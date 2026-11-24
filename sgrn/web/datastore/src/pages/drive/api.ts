// @/pages/drive/api.ts

import { authenticatedFetch, jsonBody, readBody } from "@/backend/api/fetcher";
import type { BulkActionRequest, MoveItemRequest } from "@sgrn/datastore/requests";
import { StorageBackendApiEndpoints } from "@/backend/endpoints";
import { ResumableUploader, type ChunkUploadProgress, type ResumableUploadReceipt } from "@/lib/resumableUploader";
import type { SgrnResult, CreateDirectoryResponse, MoveItemResponse, DeleteResponse } from "@sgrn/types";
import type { DriveListing } from "@/pages/drive/types";
import { ErrorScope } from "@sgrn/types";

export type StorageScope = "personal" | "automated_services" | "users" | "domains";

function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

function readApiFailure<T>(value: unknown, fallback: string, defaultScope: ErrorScope = ErrorScope.Unknown): SgrnResult<T> {
    const payload = isRecord(value) ? value : {};
    const message = typeof payload.error === "string" && payload.error.trim() ? payload.error : fallback;
    const scope =
        typeof payload.scope === "string" && Object.values(ErrorScope).includes(payload.scope as ErrorScope)
            ? (payload.scope as ErrorScope)
            : defaultScope;
    return { error: message, scope };
}

function isDriveListing(value: unknown): value is DriveListing {
    if (!isRecord(value)) return false;

    return (
        typeof value.path === "string" &&
        Array.isArray(value.folders) &&
        Array.isArray(value.files) &&
        typeof value.total_items === "number" &&
        typeof value.total_folders === "number" &&
        typeof value.total_files === "number" &&
        typeof value.page === "number" &&
        typeof value.page_size === "number" &&
        typeof value.total_pages === "number"
    );
}

function relativeUploadName(file: File): string {
    const relativePath = (file as File & { webkitRelativePath?: string }).webkitRelativePath;
    return relativePath?.trim() || file.name;
}

export interface DriveBatchUploadItemReceipt extends DriveUploadReceipt {
    success?: boolean;
    error?: string;
}

export interface DriveBatchUploadReceipt {
    success_count: number;
    fail_count: number;
    /** Results retain the order of the submitted files. */
    results: DriveBatchUploadItemReceipt[];
}

function decodeBatchUploadReceipt(value: unknown): DriveBatchUploadReceipt | null {
    if (!isRecord(value)) return null;
    const successCount = value.success_count;
    const failCount = value.fail_count;
    const rawResults = value.results;
    if (
        typeof successCount !== "number" ||
        !Number.isSafeInteger(successCount) ||
        successCount < 0 ||
        typeof failCount !== "number" ||
        !Number.isSafeInteger(failCount) ||
        failCount < 0 ||
        !Array.isArray(rawResults) ||
        !rawResults.every(isRecord)
    )
        return null;

    const results = rawResults.map((item) => ({
        ...(typeof item.success === "boolean" ? { success: item.success } : {}),
        ...(typeof item.error === "string" ? { error: item.error } : {}),
        ...(typeof item.upload_mode === "string" ? { upload_mode: item.upload_mode } : {}),
        ...(typeof item.part_count === "number" || item.part_count === null ? { part_count: item.part_count as number | null } : {}),
        ...(typeof item.part_size_bytes === "number" || item.part_size_bytes === null
            ? { part_size_bytes: item.part_size_bytes as number | null }
            : {}),
        ...(typeof item.file_id === "number" ? { file_id: item.file_id } : {}),
        ...(typeof item.object_id === "number" ? { object_id: item.object_id } : {}),
        ...(typeof item.key === "string" ? { key: item.key } : {}),
    }));

    return { success_count: successCount, fail_count: failCount, results };
}

/** Columns the backend whitelists for ordering the drive listing. */
export type DriveSortKey = "name" | "size" | "modified";
export type DriveSortOrder = "asc" | "desc";

/**
 * List directory contents at the given virtual path under a specific scope.
 *
 * Scope contract:
 * - `personal` is always the caller's own session root.
 * - `users` is the admin namespace: `/` lists users, `/email` enters that user's tree.
 * - `automated_services` follows the same pattern but is keyed by service token.
 *   Root entries include the service token in `name` and the service name in
 *   `display_name` so the UI can show both without changing navigation.
 */
export async function listDirectory(
    t_path: string = "/",
    t_scope: StorageScope = "personal",
    t_page: number = 1,
    t_limit: number = 20,
    t_search: string = "",
    t_sort: DriveSortKey = "name",
    t_order: DriveSortOrder = "asc",
): Promise<SgrnResult<DriveListing>> {
    try {
        const params = new URLSearchParams({
            path: t_path,
            scope: t_scope,
            page: t_page.toString(),
            limit: t_limit.toString(),
            search: t_search,
            sort: t_sort,
            order: t_order,
        });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_LIST}?${params.toString()}`);
        const data = await readBody<unknown>(res, null);

        if (!res.ok) return readApiFailure(data, `Failed to list directory: ${res.status}`);
        if (!isDriveListing(data)) {
            return { error: "The datastore returned an invalid directory listing.", scope: ErrorScope.Unknown };
        }

        return { data };
    } catch (e) {
        return { error: `Failed to list directory: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Download a file by its virtual path and scope.
 */
export async function downloadDriveFile(
    t_file_path: string,
    t_file_name: string,
    t_scope: StorageScope = "personal",
    t_start_byte: number = 0,
    onProgress?: (receivedBytes: number, totalBytes: number) => void,
): Promise<SgrnResult<void>> {
    try {
        const encoded_path = t_file_path.startsWith("/") ? t_file_path.slice(1) : t_file_path;
        const params = new URLSearchParams({ path: encoded_path, scope: t_scope });
        const headers: Record<string, string> = {};
        if (t_start_byte > 0) {
            headers["Range"] = `bytes=${t_start_byte}-`;
        }

        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`, { headers });

        if (!res.ok && res.status !== 206) {
            const err_data = await readBody<unknown>(res, null);
            return readApiFailure(err_data, `Download failed: ${res.status}`, ErrorScope.Network);
        }

        const disposition = res.headers.get("Content-Disposition") ?? "";
        const match = disposition.match(/filename="([^"]+)"/);
        const download_name = match?.[1] ?? t_file_name;

        const contentLengthHeader = res.headers.get("Content-Length");
        const totalBytes = contentLengthHeader ? parseInt(contentLengthHeader, 10) : 0;

        let blob: Blob;

        if (res.body && typeof res.body.getReader === "function") {
            const reader = res.body.getReader();
            const chunks: BlobPart[] = [];
            let receivedBytes = 0;

            while (true) {
                const { done, value } = await reader.read();
                if (done) break;
                if (value) {
                    chunks.push(value);
                    receivedBytes += value.length;
                    if (onProgress) {
                        onProgress(receivedBytes, totalBytes);
                    }
                }
            }
            blob = new Blob(chunks);
        } else {
            blob = await res.blob();
            if (onProgress) {
                onProgress(blob.size, blob.size);
            }
        }

        const object_url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = object_url;
        a.download = download_name;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        window.setTimeout(() => URL.revokeObjectURL(object_url), 1000);

        return { data: undefined };
    } catch (e) {
        return { error: `Download failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Fetch a file content as ArrayBuffer for preview.
 */
export async function fetchDriveFileContent(
    t_file_path: string,
    t_scope: StorageScope = "personal",
    t_max_bytes?: number,
): Promise<SgrnResult<ArrayBuffer>> {
    try {
        const encoded_path = t_file_path.startsWith("/") ? t_file_path.slice(1) : t_file_path;
        const params = new URLSearchParams({ path: encoded_path, scope: t_scope });
        const headers: Record<string, string> = {};
        if (typeof t_max_bytes === "number" && t_max_bytes > 0) {
            headers["Range"] = `bytes=0-${t_max_bytes - 1}`;
        }

        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`, { headers });

        if (!res.ok && res.status !== 206) {
            const err_data = await readBody<unknown>(res, null);
            return readApiFailure(err_data, `Fetch failed: ${res.status}`, ErrorScope.Network);
        }

        const buffer = await res.arrayBuffer();

        // Transfer-integrity check: the server stamps the *stored* entity size
        // (post-decryption bytes, compressed or not) in X-Original-Size. A
        // short body on a full (200) response means truncation in transit.
        const original_size = res.headers.get("X-Original-Size");
        if (res.status !== 206 && original_size) {
            const expected = Number(original_size);
            if (Number.isFinite(expected) && expected > 0 && buffer.byteLength !== expected) {
                return {
                    error: `File data truncated in transit: received ${buffer.byteLength} of ${expected} bytes`,
                    scope: ErrorScope.Network,
                };
            }
        }
        return { data: buffer };
    } catch (e) {
        return { error: `Fetch failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

export interface DriveUploadReceipt {
    upload_mode?: string;
    part_count?: number | null;
    part_size_bytes?: number | null;
    file_id?: number;
    object_id?: number;
    key?: string;
}

/** Human-readable chunking trace for an upload receipt. */
export function describeStorageTrace(t_upload_mode?: string, t_part_count?: number | null): string {
    return t_upload_mode === "multipart" ? ` — stored as multipart · ${t_part_count ?? "?"} parts` : " — stored as single PUT";
}

/** Batch summary suffix: how many results went multipart + total parts. */
export function summarizeBatchTrace(t_results: readonly DriveBatchUploadItemReceipt[]): string {
    const multi = t_results.filter((r) => r?.upload_mode === "multipart");
    if (multi.length === 0) return "";
    const parts = multi.reduce((n, r) => n + (typeof r?.part_count === "number" ? r.part_count : 0), 0);
    return ` · ${multi.length} stored as multipart (${parts} parts)`;
}

/**
 * Upload a file to a specific directory path and scope.
 * Resolves with the backend upload receipt (chunking trace included).
 */
export async function uploadDriveFile(
    t_directory_path: string,
    t_file: File,
    t_scope: StorageScope = "personal",
    t_on_progress?: (percent: number) => void,
): Promise<SgrnResult<DriveUploadReceipt>> {
    try {
        const token = sessionStorage.getItem("SGRN-TOKEN");

        // Send the directory path as-is (with leading slash). The backend's
        // normalizePath handles normalization. The backend handler also joins
        // the base path with the file's webkitRelativePath when it contains
        // slashes, so we must NOT append it here to avoid double-appending.
        const target_path = t_directory_path || "/";

        return new Promise((resolve) => {
            const xhr = new XMLHttpRequest();
            const params = new URLSearchParams({ path: target_path, scope: t_scope });
            xhr.open("POST", `${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`);

            if (!token) {
                return resolve({
                    error: "Unauthorized: Missing Authorization Header",
                    scope: ErrorScope.Authentication,
                });
            }
            xhr.setRequestHeader("Authorization", `Bearer ${token}`);

            const form_data = new FormData();
            // Use the webkitRelativePath as the filename to preserve directory
            // hierarchy. The backend reads this from the Content-Disposition
            // header and joins it with the base path when it contains slashes.
            const raw_name = relativeUploadName(t_file);
            const upload_name = encodeURIComponent(raw_name);
            form_data.append("file", t_file, upload_name);

            xhr.upload.onprogress = (event) => {
                if (event.lengthComputable && t_on_progress) {
                    t_on_progress(Math.floor((event.loaded / event.total) * 100));
                }
            };

            xhr.onload = () => {
                if (xhr.status >= 200 && xhr.status < 300) {
                    let receipt: DriveUploadReceipt = {};
                    try {
                        const parsed: unknown = JSON.parse(xhr.responseText);
                        if (isRecord(parsed)) {
                            receipt = {
                                ...(typeof parsed.upload_mode === "string" ? { upload_mode: parsed.upload_mode } : {}),
                                ...(typeof parsed.part_count === "number" || parsed.part_count === null
                                    ? { part_count: parsed.part_count as number | null }
                                    : {}),
                                ...(typeof parsed.part_size_bytes === "number" || parsed.part_size_bytes === null
                                    ? { part_size_bytes: parsed.part_size_bytes as number | null }
                                    : {}),
                                ...(typeof parsed.file_id === "number" ? { file_id: parsed.file_id } : {}),
                                ...(typeof parsed.object_id === "number" ? { object_id: parsed.object_id } : {}),
                                ...(typeof parsed.key === "string" ? { key: parsed.key } : {}),
                            };
                        }
                    } catch {
                        // A successful empty receipt is tolerated for legacy datastore builds.
                    }
                    resolve({ data: receipt });
                } else {
                    let body: unknown = null;
                    try {
                        body = JSON.parse(xhr.responseText) as unknown;
                    } catch {
                        /* use status fallback */
                    }
                    resolve(readApiFailure(body, `Upload failed: ${xhr.status}`, ErrorScope.Network));
                }
            };

            xhr.onerror = () => resolve({ error: "Network error", scope: ErrorScope.Network });
            xhr.onabort = () => resolve({ error: "Upload aborted", scope: ErrorScope.Network });
            xhr.ontimeout = () => resolve({ error: "Upload timed out", scope: ErrorScope.Network });
            xhr.send(form_data);
        });
    } catch (e) {
        return { error: `Upload failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

export async function uploadResumableDriveFile(
    t_directory_path: string,
    t_file: File,
    t_scope: StorageScope = "personal",
    t_on_progress?: (progress: ChunkUploadProgress) => void,
    t_encrypt: boolean = false,
): Promise<SgrnResult<ResumableUploadReceipt | undefined>> {
    try {
        const uploader = new ResumableUploader(t_file, t_directory_path, t_scope, 5 * 1024 * 1024, t_encrypt);
        if (t_on_progress) {
            uploader.onProgress(t_on_progress);
        }
        const success = await uploader.start(authenticatedFetch);
        if (success) {
            const server_encrypted = uploader.serverIsEncrypted;
            return { data: server_encrypted === null ? undefined : { is_encrypted: server_encrypted } };
        }
        return { error: "Resumable upload failed", scope: ErrorScope.Network };
    } catch (e) {
        return { error: `Resumable upload failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

export async function uploadDriveFilesBatch(
    t_directory_path: string,
    t_files: File[],
    t_scope: StorageScope = "personal",
    t_on_progress?: (percent: number) => void,
): Promise<SgrnResult<DriveBatchUploadReceipt>> {
    try {
        const token = sessionStorage.getItem("SGRN-TOKEN");
        if (!token) {
            return { error: "Unauthorized: Missing Authorization Header", scope: ErrorScope.Authentication };
        }

        const encoded_path = t_directory_path || "/";
        const total_files = t_files.length;
        if (total_files === 0) return { data: { success_count: 0, fail_count: 0, results: [] } };

        // Chunking parameters for scale
        const MAX_CHUNK_SIZE_BYTES = 50 * 1024 * 1024; // 50MB max per request to stay under 100MB backend limit
        const CONCURRENT_UPLOADS = 3; // Up to 3 simultaneous chunk requests

        let success_count = 0;
        let fail_count = 0;
        const resultsByChunk: DriveBatchUploadItemReceipt[][] = [];
        let completed_files = 0;

        // Split files into chunks based on size
        const chunks: File[][] = [];
        let current_chunk: File[] = [];
        let current_chunk_size = 0;

        for (const file of t_files) {
            // If adding this file would exceed the chunk limit (and the chunk is not empty)
            if (current_chunk.length > 0 && current_chunk_size + file.size > MAX_CHUNK_SIZE_BYTES) {
                chunks.push(current_chunk);
                current_chunk = [];
                current_chunk_size = 0;
            }
            current_chunk.push(file);
            current_chunk_size += file.size;
        }
        if (current_chunk.length > 0) {
            chunks.push(current_chunk);
        }

        // Helper to upload a single chunk
        const uploadChunk = async (chunk: File[], chunkIndex: number): Promise<void> => {
            return new Promise<void>((resolve) => {
                const xhr = new XMLHttpRequest();
                const params = new URLSearchParams({ path: encoded_path, scope: t_scope });
                xhr.open("POST", `${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`);
                xhr.setRequestHeader("Authorization", `Bearer ${token}`);

                const form_data = new FormData();
                for (const file of chunk) {
                    const raw_name = relativeUploadName(file);
                    const name = encodeURIComponent(raw_name);
                    form_data.append("files", file, name);
                }

                xhr.onload = () => {
                    completed_files += chunk.length;
                    if (t_on_progress) {
                        t_on_progress(Math.floor((completed_files / total_files) * 100));
                    }

                    if (xhr.status >= 200 && xhr.status < 300) {
                        let parsed: unknown = null;
                        try {
                            parsed = JSON.parse(xhr.responseText) as unknown;
                        } catch {
                            /* invalid batch body */
                        }
                        const receipt = decodeBatchUploadReceipt(parsed);
                        if (
                            receipt &&
                            receipt.results.length === chunk.length &&
                            receipt.success_count + receipt.fail_count === chunk.length
                        ) {
                            success_count += receipt.success_count;
                            fail_count += receipt.fail_count;
                            resultsByChunk[chunkIndex] = receipt.results;
                        } else {
                            fail_count += chunk.length;
                            resultsByChunk[chunkIndex] = chunk.map(() => ({ success: false, error: "Invalid batch upload response" }));
                        }
                    } else {
                        fail_count += chunk.length;
                        let body: unknown = null;
                        try {
                            body = JSON.parse(xhr.responseText) as unknown;
                        } catch {
                            /* status fallback */
                        }
                        const failure = readApiFailure<never>(body, `Batch request failed: ${xhr.status}`, ErrorScope.Network);
                        resultsByChunk[chunkIndex] = chunk.map(() => ({ success: false, error: failure.error }));
                    }
                    resolve();
                };

                const failChunk = (message: string) => {
                    completed_files += chunk.length;
                    fail_count += chunk.length;
                    resultsByChunk[chunkIndex] = chunk.map(() => ({ success: false, error: message }));
                    if (t_on_progress) t_on_progress(Math.floor((completed_files / total_files) * 100));
                    resolve();
                };
                xhr.onerror = () => failChunk("Network error");
                xhr.onabort = () => failChunk("Upload aborted");
                xhr.ontimeout = () => failChunk("Batch upload timed out");

                xhr.send(form_data);
            });
        };

        // Execute chunks with limited concurrency
        let current_chunk_idx = 0;
        const executeNext = async (): Promise<void> => {
            if (current_chunk_idx >= chunks.length) return;
            const chunkIndex = current_chunk_idx++;
            const chunk = chunks[chunkIndex];
            await uploadChunk(chunk, chunkIndex);
            return executeNext();
        };

        const workers = Array.from({ length: Math.min(CONCURRENT_UPLOADS, chunks.length) }, () => executeNext());
        await Promise.all(workers);

        return { data: { success_count, fail_count, results: resultsByChunk.flat() } };
    } catch (e) {
        return { error: `Batch upload failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Create an empty directory at the given path and scope.
 * The backend resolves the path within the current storage namespace.
 */
export async function createDirectory(t_path: string, t_scope: StorageScope = "personal"): Promise<SgrnResult<CreateDirectoryResponse>> {
    try {
        const params = new URLSearchParams({ path: t_path, scope: t_scope });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_MKDIR}?${params.toString()}`, { method: "POST" });
        const data = await readBody<unknown>(res, {});

        if (!res.ok) {
            return readApiFailure(data, `Failed to create directory: ${res.status}`);
        }

        return { data: data as CreateDirectoryResponse };
    } catch (e) {
        return { error: `Failed to create directory: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Move or rename a drive item (file or folder).
 * `parent_id = null` moves the item back to the current namespace root.
 */
export async function moveDriveItem(
    t_id: number,
    t_type: "file" | "folder",
    t_new_name?: string,
    t_new_parent_id?: number | null,
    t_scope: StorageScope = "personal",
): Promise<SgrnResult<MoveItemResponse>> {
    try {
        const params = new URLSearchParams({ id: t_id.toString(), type: t_type, scope: t_scope });
        const body: MoveItemRequest = {};
        if (t_new_name) body.name = t_new_name;
        if (t_new_parent_id !== undefined) body.parent_id = t_new_parent_id;

        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_MOVE}?${params.toString()}`, {
            method: "PATCH",
            ...jsonBody<MoveItemRequest>(body),
        });

        const data = await readBody<unknown>(res, {});
        if (!res.ok) {
            return readApiFailure(data, `Failed to move ${t_type}: ${res.status}`);
        }

        return { data: data as MoveItemResponse };
    } catch (e) {
        return { error: `Failed to move: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Delete a drive item (file or folder).
 */
export async function deleteDriveItem(
    t_id: number,
    t_type: "file" | "folder",
    t_scope: StorageScope = "personal",
): Promise<SgrnResult<DeleteResponse>> {
    try {
        const params = new URLSearchParams({ id: t_id.toString(), type: t_type, scope: t_scope });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_DELETE}?${params.toString()}`, { method: "DELETE" });

        const data = await readBody<unknown>(res, {});
        if (!res.ok) {
            return readApiFailure(data, `Failed to delete ${t_type}: ${res.status}`);
        }

        return { data: data as DeleteResponse };
    } catch (e) {
        return { error: `Failed to delete: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Renames a file or folder. The scope must match the listing the item came
 * from, otherwise the datastore resolves the id inside the wrong namespace.
 */
export async function renameItem(
    id: number | string,
    type: "file" | "folder",
    newName: string,
    t_scope: StorageScope = "personal",
): Promise<SgrnResult<MoveItemResponse>> {
    try {
        const params = new URLSearchParams({ id: id.toString(), type: type, scope: t_scope });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_MOVE}?${params.toString()}`, {
            method: "PATCH",
            ...jsonBody<MoveItemRequest>({ new_name: newName }),
        });

        const data = await readBody<unknown>(res, {});
        if (!res.ok) {
            return readApiFailure(data, `Failed to rename ${type}: ${res.status}`);
        }

        return { data: data as MoveItemResponse };
    } catch (e) {
        return { error: `Failed to rename: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

export interface DriveBulkActionReceipt {
    success_count: number;
    total: number;
}

function decodeBulkActionReceipt(value: unknown): DriveBulkActionReceipt | null {
    if (!isRecord(value)) return null;
    const successCount = value.success_count;
    const total = value.total;
    if (
        typeof successCount !== "number" ||
        !Number.isSafeInteger(successCount) ||
        successCount < 0 ||
        typeof total !== "number" ||
        !Number.isSafeInteger(total) ||
        total < successCount
    )
        return null;
    return { success_count: successCount, total };
}

/**
 * Performs a bulk action (move or delete) on multiple items.
 */
export async function bulkAction(
    action: "move" | "delete",
    items: { id: number | string; type: "file" | "folder" }[],
    targetParentId?: number | null,
    t_scope: StorageScope = "personal",
): Promise<SgrnResult<DriveBulkActionReceipt>> {
    try {
        const params = new URLSearchParams({ scope: t_scope });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_BULK}?${params.toString()}`, {
            method: "POST",
            ...jsonBody<BulkActionRequest>({
                action,
                items,
                target_parent_id: targetParentId,
            }),
        });

        const data = await readBody<unknown>(res, {});
        if (!res.ok) {
            return readApiFailure(data, `Failed to perform bulk ${action}: ${res.status}`);
        }

        const receipt = decodeBulkActionReceipt(data);
        if (!receipt) {
            return { error: "The datastore returned an invalid bulk-action receipt.", scope: ErrorScope.Unknown };
        }
        return { data: receipt };
    } catch (e) {
        return { error: `Failed to perform bulk action: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}
