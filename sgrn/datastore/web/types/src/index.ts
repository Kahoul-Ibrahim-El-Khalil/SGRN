// ==========================================
// 1. Core Metadata Types
// ==========================================

export interface FileMetadata {
    id: number;
    name: string;
    object_id: number;
    session_id: number;
    extension: string;
    directory_id?: number | null;
    created_at: string;

    // Optional Extended Schema details
    full_path?: string;
    directory_path?: string;
    user_id?: number;
    domain?: string | null;
    bucket: string;
    minio_key: string;
    size: number;
    object_created_at?: string;
    mime_type?: string | null;
    object?: StorageObject;
}

export interface StorageObject {
    id: number;
    bucket?: string;
    key?: string;
    size: number;
    original_size: number;
    is_compressed: boolean;
    compression_algorithm: string | null;
    compression_level: number | null;
    created_at?: string;
    deleted_at?: string | null;
}

export interface UploadResult {
    file_id?: number;
    object_id?: number;
    file?: string; // Original filename (web)
    original_filename?: string;
    virtual_path?: string;
    success?: boolean;
    status?: number;
    error?: string;
    already_exists?: boolean;
    is_duplicate?: boolean; // alias for already_exists
    deduplicated?: boolean;
    identity?: {
        key: string;
        hash: string;
        extension: string;
        mime_type: string;
        is_compressed: boolean;
        original_size: number;
        final_size: number;
        compression_algorithm?: string;
        compression_level?: number;
    };
    uploaded_at?: string;
    [key: string]: unknown;
}

export interface UploadConstraints {
    chunk_size?: number;
    max_session_size?: number;
    max_file_size_mb?: number;
    parallel_chunks?: number;
    max_retries?: number;
    units?: string;
    version?: string;
    description?: string;
    allowed_extensions?: string[];
    compression_level?: number;
    threshold_presigned_mb?: number;
    [key: string]: unknown;
}

// ==========================================
// 2. Identity and Session Types
// ==========================================

export interface Role {
    name: string;
    code?: number;
}

export interface Domain {
    id: number;
    name: string;
}

export interface Organisation {
    id: number;
    name: string;
}

export interface User {
    id: number;
    user_id?: number; // alias
    first_name: string;
    family_name: string;
    email: string;
    phone_number?: string | null;
    role?: string | Role;
    domain?: Domain | string;
    organisation?: Organisation | string;
    created_at?: string;
    is_active?: boolean;
    status?: string;
}

export interface Credentials {
    token: string;
    secret: string;
}

export interface AutomatedServiceSession {
    automated_service_id: number;
    automated_service?: Record<string, unknown>;
    session_id?: string;
}

// ==========================================
// 3. System and API Response
// ==========================================

export const ErrorScope = {
    Runtime: "Runtime",
    Database: "Database",
    Redis: "Redis",
    Minio: "Minio",
    Postgrest: "Postgrest",
    Compression: "Compression",
    Hashing: "Hashing",
    ApplicationLogic: "ApplicationLogic",
    Authentication: "Authentication",
    Authorization: "Authorization",
    FileSystem: "FileSystem",
    Network: "Network",
    Filter: "Filter",
    Service: "Service",
    Allocation: "Allocation",
    PLC: "PLC",
    Unknown: "Unknown",
} as const;

export type ErrorScope = (typeof ErrorScope)[keyof typeof ErrorScope];

export interface SgrnError {
    error: string;
    scope: ErrorScope;
}

export type SgrnResult<T> =
    | {
          data: T;
          error?: never;
          scope?: never;
      }
    | {
          error: string;
          scope: ErrorScope;
          data?: never;
      };

/**
 * Type guard to check if a result is successful
 */
export function isSuccess<T>(result: SgrnResult<T>): result is {
    data: T;
    error?: never;
} {
    return result.error === undefined;
}

/**
 * Type guard to check if a result is an error
 */
export function isError<T>(result: SgrnResult<T>): result is {
    error: string;
    scope: ErrorScope;
    data?: never;
} {
    return result.error !== undefined;
}

// ==========================================
// 4. Drive and Storage Types
// ==========================================

export interface DriveFolder {
    id?: number;
    name: string;
    path: string;
    created_at?: string;
    display_name?: string;
    token?: string;
    kind?: string;
    virtual_size?: number;
    real_size?: number;
    count_sub_files?: number;
    count_sub_directories?: number;
}

export interface DrivePathNode {
    id?: number | null;
    name: string;
    path: string;
    display_name?: string;
}

export interface DriveFile {
    id: number;
    name: string;
    path: string;
    extension: string;
    size: number; // Compressed size
    original_size: number;
    created_at: string;
}

export interface DirectoryListing {
    path: string;
    trail?: DrivePathNode[];
    folders: DriveFolder[];
    files: DriveFile[];
    total_items: number;
    total_folders: number;
    total_files: number;
    page: number;
    page_size: number;
    total_pages: number;
}

export interface Directory {
    id: number;
    parent_id?: number | null;
    user_id?: number | null;
    automated_service_id?: number | null;
    session_id?: number;
    name: string;
    path: string;
    virtual_size: number;
    real_size: number;
    count_sub_files?: number;
    count_sub_directories?: number;
    depth?: number;
    created_at?: string;
}

export interface ApiError {
    status: number;
    message: string;
}

export interface ApiResponse<T = any> {
    success: boolean;
    result?: string;
    data?: T;
    error?: string;
}
