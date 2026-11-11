import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import type { SgrnResult } from "@sgrn/types";

export interface RegisterUserPayload {
    email: string;
    password: string;
    organisation: string;
    first_name: string;
    family_name: string;
}

export interface RegisterUserResp {
    success: boolean;
    user: import("@/pages/signin/types").User;
}

export async function registerUser(t_payload: RegisterUserPayload): Promise<SgrnResult<RegisterUserResp>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_ADMIN, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(t_payload),
    });
    return await processResponse<RegisterUserResp>(res);
}

export async function setUserStatus(t_id: number, t_status: "active" | "suspended"): Promise<SgrnResult<{ success: boolean }>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_STATUS(t_id), {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ status: t_status }),
    });
    return await processResponse<{ success: boolean }>(res);
}

export async function resetUserPassword(t_id: number, t_new_password: string): Promise<SgrnResult<{ success: boolean }>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_PASSWORD(t_id), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ new_password: t_new_password }),
    });
    return await processResponse<{ success: boolean }>(res);
}

export async function deleteUser(t_id: number): Promise<SgrnResult<{ success: boolean }>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_ITEM(t_id), { method: "DELETE" });
    return await processResponse<{ success: boolean }>(res);
}

export async function deleteAutomatedService(t_id: number): Promise<SgrnResult<{ success: boolean }>> {
    const res = await authenticatedFetch(`${AdminBackendApiEndpoints.WEBHOOK_ITEM(t_id)}`, { method: "DELETE" });
    return await processResponse<{ success: boolean }>(res);
}
