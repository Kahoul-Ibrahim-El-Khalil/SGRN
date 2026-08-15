// src/types/index.ts

// -----------------------------------------------------------------------
// User and Authentication Types
// -----------------------------------------------------------------------
export const UserStatus = {
    ACTIVE: "active",
    SUSPENDED: "suspended",
    INVITED: "invited",
} as const;

export type UserStatus = (typeof UserStatus)[keyof typeof UserStatus];

export interface User {
    id: number;
    first_name: string;
    family_name: string;
    email: string;
    status?: UserStatus | string;
    domain?: string;
    phone_number?: string | null;

    total_virtual_size?: number;
    total_real_size?: number;
    storage_limit?: number | null;
    total_entry_count?: number;
    entry_count_limit?: number | null;
}

// -----------------------------------------------------------------------
// File Upload Types
// -----------------------------------------------------------------------
export type UploadMode = "single" | "batch" | "archive";

export type UploadStatus = "idle" | "uploading" | "success" | "error" | "chunk_required";

export interface UploadConstraints {
    chunking_file_threshold: number; // in bytes
    max_file_size: number; // in bytes
    allowed_extensions?: string[];
    max_batch_size?: number;
}

// -----------------------------------------------------------------------
// Window Management Types
// -----------------------------------------------------------------------
export interface WindowProps {
    id: string;
    title: string;
    icon: React.ComponentType<{ size?: number; className?: string }>;
    is_open: boolean;
    is_minimized: boolean;
    component: React.ReactNode;
}

// -----------------------------------------------------------------------
// API Response Types
// -----------------------------------------------------------------------
export interface ApiResponse<T = any> {
    success: boolean;
    result?: string;
    data?: T;
    error?: string;
}

export interface UploadResponse {
    success: boolean;
    result: string;
    uploaded_files?: string[];
    failed_files?: Array<{
        filename: string;
        error: string;
    }>;
}
