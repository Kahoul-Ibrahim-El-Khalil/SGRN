// @/pages/drive/api.ts

import { authenticatedFetch } from "@/backend/api/fetcher";
import { StorageBackendApiEndpoints } from "@/backend/endpoints";
import type { DirectoryListing, SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export type StorageScope = "personal" | "automated_services" | "users" | "domains";

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
): Promise<SgrnResult<DirectoryListing>> {
    try {
        const params = new URLSearchParams({
            path: t_path,
            scope: t_scope,
            page: t_page.toString(),
            limit: t_limit.toString(),
            search: t_search,
        });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_LIST}?${params.toString()}`);
        const data = await res.json();

        if (!res.ok) {
            return { error: data.error || `Failed to list directory: ${res.status}`, scope: data.scope || ErrorScope.Unknown };
        }

        return { data: data as DirectoryListing };
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
): Promise<SgrnResult<void>> {
    try {
        const encoded_path = t_file_path.startsWith("/") ? t_file_path.slice(1) : t_file_path;
        const params = new URLSearchParams({ path: encoded_path, scope: t_scope });

        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`);

        if (!res.ok) {
            const err_data = await res.json().catch(() => null);
            return { error: err_data?.error || `Download failed: ${res.status}`, scope: err_data?.scope || ErrorScope.Network };
        }

        const disposition = res.headers.get("Content-Disposition") ?? "";
        const match = disposition.match(/filename="([^"]+)"/);
        const download_name = match?.[1] ?? t_file_name;

        const blob = await res.blob();
        const object_url = URL.createObjectURL(blob);

        const a = document.createElement("a");
        a.href = object_url;
        a.download = download_name;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(object_url);

        return { data: undefined };
    } catch (e) {
        return { error: `Download failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Fetch a file content as ArrayBuffer for preview.
 */
export async function fetchDriveFileContent(t_file_path: string, t_scope: StorageScope = "personal"): Promise<SgrnResult<ArrayBuffer>> {
    try {
        const encoded_path = t_file_path.startsWith("/") ? t_file_path.slice(1) : t_file_path;
        const params = new URLSearchParams({ path: encoded_path, scope: t_scope });

        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`);

        if (!res.ok) {
            const err_data = await res.json().catch(() => null);
            return { error: err_data?.error || `Fetch failed: ${res.status}`, scope: err_data?.scope || ErrorScope.Network };
        }

        const buffer = await res.arrayBuffer();
        return { data: buffer };
    } catch (e) {
        return { error: `Fetch failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Upload a file to a specific directory path and scope.
 */
export async function uploadDriveFile(
    t_directory_path: string,
    t_file: File,
    t_scope: StorageScope = "personal",
    t_on_progress?: (percent: number) => void,
): Promise<SgrnResult<void>> {
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
            const raw_name = (t_file as any).webkitRelativePath || t_file.name;
            const upload_name = encodeURIComponent(raw_name);
            form_data.append("file", t_file, upload_name);

            xhr.upload.onprogress = (event) => {
                if (event.lengthComputable && t_on_progress) {
                    t_on_progress(Math.floor((event.loaded / event.total) * 100));
                }
            };

            xhr.onload = () => {
                if (xhr.status >= 200 && xhr.status < 300) {
                    resolve({ data: undefined });
                } else {
                    let error_msg = `Upload failed: ${xhr.status}`;
                    let scope = ErrorScope.Network;
                    try {
                        const resp = JSON.parse(xhr.responseText);
                        if (resp.error) error_msg = resp.error;
                        if (resp.scope) scope = resp.scope;
                    } catch (_) {}
                    resolve({ error: error_msg, scope });
                }
            };

            xhr.onerror = () => resolve({ error: "Network error", scope: ErrorScope.Network });
            xhr.send(form_data);
        });
    } catch (e) {
        return { error: `Upload failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

export async function uploadDriveFilesBatch(
    t_directory_path: string,
    t_files: File[],
    t_scope: StorageScope = "personal",
    t_on_progress?: (percent: number) => void,
): Promise<SgrnResult<{ success_count: number; fail_count: number; results: any[] }>> {
    try {
        const token = sessionStorage.getItem("SGRN-TOKEN");
        if (!token) {
            return { error: "Unauthorized: Missing Authorization Header", scope: ErrorScope.Authentication };
        }

        const encoded_path = t_directory_path || "/";
        const total_files = t_files.length;

        // Chunking parameters for scale
        const MAX_CHUNK_SIZE_BYTES = 50 * 1024 * 1024; // 50MB max per request to stay under 100MB backend limit
        const CONCURRENT_UPLOADS = 3; // Up to 3 simultaneous chunk requests

        let success_count = 0;
        let fail_count = 0;
        const all_results: any[] = [];
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
        const uploadChunk = async (chunk: File[]) => {
            return new Promise<void>((resolve) => {
                const xhr = new XMLHttpRequest();
                const params = new URLSearchParams({ path: encoded_path, scope: t_scope });
                xhr.open("POST", `${StorageBackendApiEndpoints.PATH_BASE}?${params.toString()}`);
                xhr.setRequestHeader("Authorization", `Bearer ${token}`);

                const form_data = new FormData();
                for (const file of chunk) {
                    const raw_name = (file as any).webkitRelativePath || file.name;
                    const name = encodeURIComponent(raw_name);
                    form_data.append("files", file, name);
                }

                xhr.onload = () => {
                    completed_files += chunk.length;
                    if (t_on_progress) {
                        t_on_progress(Math.floor((completed_files / total_files) * 100));
                    }

                    if (xhr.status >= 200 && xhr.status < 300) {
                        try {
                            const resp = JSON.parse(xhr.responseText);
                            success_count += resp.success_count || 0;
                            fail_count += resp.fail_count || 0;
                            if (resp.results) all_results.push(...resp.results);
                        } catch (_) {
                            fail_count += chunk.length;
                        }
                    } else {
                        fail_count += chunk.length;
                    }
                    resolve();
                };

                xhr.onerror = () => {
                    completed_files += chunk.length;
                    fail_count += chunk.length;
                    if (t_on_progress) t_on_progress(Math.floor((completed_files / total_files) * 100));
                    resolve();
                };

                xhr.send(form_data);
            });
        };

        // Execute chunks with limited concurrency
        let current_chunk_idx = 0;
        const executeNext = async (): Promise<void> => {
            if (current_chunk_idx >= chunks.length) return;
            const chunk = chunks[current_chunk_idx++];
            await uploadChunk(chunk);
            return executeNext();
        };

        const workers = Array.from({ length: Math.min(CONCURRENT_UPLOADS, chunks.length) }, () => executeNext());
        await Promise.all(workers);

        return { data: { success_count, fail_count, results: all_results } };
    } catch (e) {
        return { error: `Batch upload failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Create an empty directory at the given path and scope.
 * The backend resolves the path within the current storage namespace.
 */
export async function createDirectory(t_path: string, t_scope: StorageScope = "personal"): Promise<SgrnResult<{ directory_id?: number }>> {
    try {
        const params = new URLSearchParams({ path: t_path, scope: t_scope });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_MKDIR}?${params.toString()}`, { method: "POST" });
        const data = await res.json();

        if (!res.ok || !data.success) {
            return { error: data.error || `Failed to create directory: ${res.status}`, scope: data.scope || ErrorScope.Unknown };
        }

        return { data: { directory_id: data.directory_id } };
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
): Promise<SgrnResult<void>> {
    try {
        const params = new URLSearchParams({ id: t_id.toString(), type: t_type, scope: t_scope });
        const body: any = {};
        if (t_new_name) body.name = t_new_name;
        if (t_new_parent_id !== undefined) body.parent_id = t_new_parent_id;

        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_MOVE}?${params.toString()}`, {
            method: "PATCH",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body),
        });

        if (!res.ok) {
            const data = await res.json().catch(() => ({}));
            return { error: data.error || `Failed to move ${t_type}: ${res.status}`, scope: data.scope || ErrorScope.Unknown };
        }

        return { data: undefined };
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
): Promise<SgrnResult<void>> {
    try {
        const params = new URLSearchParams({ id: t_id.toString(), type: t_type, scope: t_scope });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_DELETE}?${params.toString()}`, { method: "DELETE" });

        if (!res.ok) {
            const data = await res.json().catch(() => ({}));
            return { error: data.error || `Failed to delete ${t_type}: ${res.status}`, scope: data.scope || ErrorScope.Unknown };
        }

        return { data: undefined };
    } catch (e) {
        return { error: `Failed to delete: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Renames a file or folder.
 */
export async function renameItem(id: number | string, type: "file" | "folder", newName: string): Promise<SgrnResult<any>> {
    try {
        const params = new URLSearchParams({ id: id.toString(), type: type });
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_MOVE}?${params.toString()}`, {
            method: "PATCH",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ new_name: newName }),
        });

        if (!res.ok) {
            const data = await res.json().catch(() => ({}));
            return { error: data.error || `Failed to rename ${type}: ${res.status}`, scope: data.scope || ErrorScope.Unknown };
        }

        return { data: await res.json().catch(() => ({})) };
    } catch (e) {
        return { error: `Failed to rename: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}

/**
 * Performs a bulk action (move or delete) on multiple items.
 */
export async function bulkAction(
    action: "move" | "delete",
    items: { id: number | string; type: "file" | "folder" }[],
    targetParentId?: number | null,
): Promise<SgrnResult<any>> {
    try {
        const res = await authenticatedFetch(`${StorageBackendApiEndpoints.DRIVE_BULK}`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
                action,
                items,
                target_parent_id: targetParentId,
            }),
        });

        if (!res.ok) {
            const data = await res.json().catch(() => ({}));
            return { error: data.error || `Failed to perform bulk ${action}: ${res.status}`, scope: data.scope || ErrorScope.Unknown };
        }

        return { data: await res.json().catch(() => ({})) };
    } catch (e) {
        return { error: `Failed to perform bulk action: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
    }
}
