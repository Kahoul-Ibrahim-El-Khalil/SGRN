import { UserStatus } from "@/types";

export interface RegisterFormData {
    first_name: string;
    family_name: string;
    email: string;
    password: string;
    organisation: string;

    status: UserStatus | string;
    domain?: string;
    phone_number?: string | null;
}
