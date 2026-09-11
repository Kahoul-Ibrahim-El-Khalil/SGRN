import { StorageBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import type { UploadResult, UploadConstraints } from "@/pages/upload/types";
import type { FileMetadata, ListFilesParams } from "@/pages/download/types";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

function normalizeFileMetadata(t_file: any): FileMetadata {
    const object = t_file?.object ?? {
        id: t_file?.object_id,
        bucket: t_file?.bucket,
        key: t_file?.key ?? t_file?.minio_key,
        size: t_file?.size ?? 0,
        original_size: t_file?.original_size ?? t_file?.size ?? 0,
        is_compressed: t_file?.is_compressed ?? false,
        compression_algorithm: t_file?.compression_algorithm ?? null,
        compression_level: t_file?.compression_level ?? null,
        created_at: t_file?.object_created_at,
    };

    return {
        ...t_file,
        minio_key: t_file?.minio_key ?? t_file?.key ?? "",
        object,
    } as FileMetadata;
}

/**
 * Fetch storage constraints from the backend
 */
export async function getStorageConstraints(): Promise<SgrnResult<UploadConstraints>> {
    try {
        const res = await authenticatedFetch(StorageBackendApiEndpoints.CONSTRAINTS);
        const result = await processResponse<any>(res);

        if (result.error) {
            return result;
        }

        const data = result.data;
        const constraints: UploadConstraints = {
            chunkSize: 5 * 1024 * 1024,
            maxSessionSize: (data.max_file_size_mb || 100) * 1024 * 1024,
            parallelChunks: 1,
            maxRetries: 3,
            units: "bytes",
            version: "v2",
            description: "S3 Storage Constraints",
            max_file_size_mb: data.max_file_size_mb,
            threshold_presigned_mb: data.compression_threshold_mb ?? data.threshold_presigned_mb,
            allowed_extensions: data.allowed_extensions || [],
        } as any;

        return { data: constraints };
    } catch (e) {
        return {
            error: `Failed to fetch constraints: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

/**
 * Upload a file using direct POST strategy (MinIO direct uploads are deprecated)
 */
export async function uploadFile(
    t_file: File,
    _t_constraints: UploadConstraints,
    t_on_progress?: (t_percent: number) => void,
): Promise<SgrnResult<UploadResult>> {
    return uploadDirect(t_file, t_on_progress);
}

export async function uploadDirect(t_file: File, t_on_progress?: (t_percent: number) => void): Promise<SgrnResult<UploadResult>> {
    try {
        const token = sessionStorage.getItem("SGRN-TOKEN") || localStorage.getItem("SGRN-TOKEN");

        if (!token) {
            return {
                error: "Authentication required: No token found. Please log in.",
                scope: ErrorScope.Authentication,
            };
        }

        return new Promise((resolve) => {
            const xhr = new XMLHttpRequest();
            // Ensure leading slash for the path (backend requirement)
            const path = t_file.name.startsWith("/") ? t_file.name : `/${t_file.name}`;
            const params = new URLSearchParams({
                path: path,
            });

            xhr.open("POST", `${StorageBackendApiEndpoints.UPLOAD}?${params.toString()}`);
            xhr.setRequestHeader("Authorization", `Bearer ${token}`);

            const form_data = new FormData();
            form_data.append("file", t_file);

            xhr.upload.onprogress = (event) => {
                if (event.lengthComputable && t_on_progress) {
                    t_on_progress(Math.floor((event.loaded / event.total) * 100));
                }
            };

            xhr.onload = () => {
                if (xhr.status >= 200 && xhr.status < 300) {
                    resolve({ data: { file: t_file.name, success: true, status: xhr.status } });
                } else {
                    let error_msg = `Upload failed: ${xhr.status}`;
                    let scope = ErrorScope.Network;
                    try {
                        const resp = JSON.parse(xhr.responseText);
                        if (resp.error) error_msg = resp.error;
                        if (resp.scope) scope = resp.scope;
                    } catch (e) {}
                    resolve({ error: error_msg, scope: scope });
                }
            };

            xhr.onerror = () => resolve({ error: "Network error during direct upload", scope: ErrorScope.Network });
            xhr.send(form_data);
        });
    } catch (e) {
        return {
            error: `Direct upload failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Runtime,
        };
    }
}

/**
 * Trigger browser download for a given file path
 */
export async function downloadFile(t_path: string, t_filename: string, t_is_compressed: boolean = false): Promise<SgrnResult<void>> {
    try {
        const url = `${StorageBackendApiEndpoints.DOWNLOAD}?path=${encodeURIComponent(t_path)}`;
        const response = await authenticatedFetch(url);

        if (!response.ok) {
            return {
                error: `Failed to fetch file: ${response.statusText}`,
                scope: ErrorScope.Network,
            };
        }

        const blob = await response.blob();
        const object_url = URL.createObjectURL(blob);

        const a = document.createElement("a");
        a.href = object_url;

        // Ensure .zst extension if compressed
        let final_filename = t_filename;
        if (t_is_compressed && !t_filename.toLowerCase().endsWith(".zst")) {
            final_filename = `${t_filename}.zst`;
        }

        a.download = final_filename;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(object_url);

        return { data: undefined };
    } catch (error) {
        return {
            error: `Download failed: ${error instanceof Error ? error.message : String(error)}`,
            scope: ErrorScope.Network,
        };
    }
}

/**
 * List files with various filters
 */
export async function listFiles(t_params: ListFilesParams): Promise<SgrnResult<FileMetadata[]>> {
    try {
        const { mode, identifier, limit = 20, offset = 0, bucket } = t_params;
        let url = "";
        const base_url = "/api/v1/postgrest/storage/files";
        const query_params = new URLSearchParams();
        query_params.append("limit", limit.toString());
        query_params.append("offset", offset.toString());
        query_params.append("order", "created_at.desc");

        if (bucket) query_params.append("bucket", `eq.${bucket}`);

        switch (mode) {
            case "user":
                query_params.append("user_id", `eq.${identifier}`);
                break;
            case "domain":
                query_params.append("domain", `eq.${identifier}`);
                break;
            case "search":
                query_params.append("name", `ilike.*${identifier}*`);
                break;
            case "path":
                query_params.append("full_path", `like.${identifier}%`);
                break;
            case "extension":
                query_params.append("extension", `eq.${identifier}`);
                break;
            case "submission":
                query_params.append("submission_id", `eq.${identifier}`);
                break;
            case "session":
                query_params.append("session_id", `eq.${identifier}`);
                break;
        }

        url = `${base_url}?${query_params.toString()}`;

        const res = await authenticatedFetch(url);
        const result = await processResponse<any>(res);

        if (result.error) {
            return result as SgrnResult<any>;
        }

        const data = result.data;
        if (Array.isArray(data)) {
            return { data: data.map(normalizeFileMetadata) };
        }
        if (data.files && Array.isArray(data.files)) {
            return { data: data.files.map(normalizeFileMetadata) };
        }

        return { data: [] };
    } catch (e) {
        return {
            error: `Failed to list files: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
