// src/types/download.ts

export interface DownloadInfo {
    success: boolean;
    metadata: {
        original_name?: string;
        bucket?: string;
        is_compressed?: boolean;
        compression_algorithm?: string;
    };
    download_context: {
        url: string;
        redirect_url: string;
        time_window: number;
    };
}

export interface ListFilesParams {
    mode: "user" | "domain" | "search" | "path" | "extension" | "submission" | "session";
    identifier?: string | number;
    limit?: number;
    offset?: number;
    bucket?: string;
}

/**
 * File metadata from GET /api/v1/storage/files/metadata
 * (served from the storage.file_details view)
 */
export type { FileMetadata } from "@sgrn/types";

/**
 * File-metadata filter parameters for GET /api/v1/storage/files/metadata.
 * Grammar is `field=op.value` (op in eq/like/ilike; `*` in like/ilike
 * patterns is a `%` wildcard), plus `order=`, `limit=` and `offset=`.
 * Served in-process by the datastore, tenant-scoped server-side.
 */
export interface FileMetadataFilterParams {
    // Filters (field=operator.value)
    user_id?: string; // e.g., "eq.42"
    domain?: string; // e.g., "eq.HR"
    session_id?: string; // e.g., "eq.10"
    extension?: string; // e.g., "eq.pdf"
    bucket?: string; // e.g., "eq.sgrn-uploads"
    name?: string; // e.g., "ilike.*invoice*"
    full_path?: string; // e.g., "like./documents/2024/%"
    directory_id?: string; // e.g., "eq.null" for root files, "eq.42" for a specific directory

    // Ordering
    order?: string; // e.g., "created_at.desc" or "name.asc"

    // Pagination
    limit?: number;
    offset?: number;
}

/**
 * Search filter types
 */
export type FilterType =
    | "name" // Filename search
    | "extension" // File extension
    | "path" // Path prefix
    | "domain" // Domain filter (admin only)
    | "user" // User filter (admin only)
    | "session"; // Session filter

/**
 * User info from session storage
 */
export interface UserInfo {
    id: number;
    user_id?: number; // Fallback field name
    email: string;
    domain?: string;
    role?: string; // Now a string field (e.g., "admin", "user") instead of nested object
}

/**
 * Download request parameters
 */
export interface DownloadParams {
    bucket: string;
    minio_key: string;
    filename: string;
}
