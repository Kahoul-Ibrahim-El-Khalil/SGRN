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
 * File metadata from api.files view
 * Matches the PostgreSQL view structure (storage.file_paths view)
 */
export type { FileMetadata } from "@sgrn/types";

/**
 * PostgREST query parameters
 */
export interface PostgRESTParams {
    // Filters (PostgREST format: field=operator.value)
    user_id?: string; // e.g., "eq.42"
    domain?: string; // e.g., "eq.HR"
    session_id?: string; // e.g., "eq.10"
    extension?: string; // e.g., "eq.pdf" or "in.(pdf,docx)"
    name?: string; // e.g., "ilike.*invoice*"
    full_path?: string; // e.g., "like./documents/2024/%"
    directory_id?: string; // e.g., "eq.null" for root files, "eq.42" for specific directory
    is_compressed?: string; // e.g., "eq.true" or "eq.false"

    // Ordering
    order?: string; // e.g., "created_at.desc" or "name.asc"

    // Pagination
    limit?: number;
    offset?: number;

    // Range (alternative to limit/offset)
    range?: string; // e.g., "0-49" for first 50 results
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
