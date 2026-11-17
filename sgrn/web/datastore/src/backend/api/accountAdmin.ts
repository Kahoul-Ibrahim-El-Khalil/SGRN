import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, jsonBody, processResponse } from "@/backend/api/fetcher";
import type { RegisterUserRequest, ResetPasswordRequest, SetStatusRequest } from "@sgrn/datastore/requests";
import type { SgrnResult, UserStatusResponse, DeletedUserResponse, DeletedServiceResponse } from "@sgrn/types";

export interface RegisterUserPayload {
    email: string;
    password: string;
    organisation: string;
    first_name: string;
    family_name: string;
}

export interface RegisterUserResp {
    user: import("@/pages/signin/types").User;
}

export async function registerUser(t_payload: RegisterUserPayload): Promise<SgrnResult<RegisterUserResp>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_ADMIN, {
        method: "POST",
        ...jsonBody<RegisterUserRequest>(t_payload),
    });
    return await processResponse<RegisterUserResp>(res);
}

export async function setUserStatus(t_id: number, t_status: "active" | "suspended"): Promise<SgrnResult<UserStatusResponse>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_STATUS(t_id), {
        method: "PATCH",
        ...jsonBody<SetStatusRequest>({ status: t_status }),
    });
    return await processResponse<UserStatusResponse>(res);
}

export async function resetUserPassword(t_id: number, t_new_password: string): Promise<SgrnResult<Record<string, never>>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_PASSWORD(t_id), {
        method: "POST",
        ...jsonBody<ResetPasswordRequest>({ new_password: t_new_password }),
    });
    return await processResponse<Record<string, never>>(res);
}

export async function deleteUser(t_id: number): Promise<SgrnResult<DeletedUserResponse>> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.ACCOUNT_ITEM(t_id), { method: "DELETE" });
    return await processResponse<DeletedUserResponse>(res);
}

export async function deleteAutomatedService(t_id: number): Promise<SgrnResult<DeletedServiceResponse>> {
    const res = await authenticatedFetch(`${AdminBackendApiEndpoints.WEBHOOK_ITEM(t_id)}`, { method: "DELETE" });
    return await processResponse<DeletedServiceResponse>(res);
}
