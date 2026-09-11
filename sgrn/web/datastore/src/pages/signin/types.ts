// types.ts
import { UserStatus } from "@/types";

export interface Role {
    name: string;
    code?: number;
}

export interface Organisation {
    id: number;
    name: string;
}

export interface User {
    id: number;
    first_name: string;
    family_name: string;
    email: string;
    phone_number: string;
    role: string | Role; // Can be string (new schema) or Role object (legacy)
    status?: UserStatus | string;

    organisation: Organisation | string;
    domain?: string;
    created_at: string;
    is_active: boolean;

    total_virtual_size?: number;
    total_real_size?: number;
    storage_limit?: number | null;
    total_entry_count?: number;
    entry_count_limit?: number | null;
}

export interface LoginResponse {
    message: string;
    token: string;
    user: User;
}
