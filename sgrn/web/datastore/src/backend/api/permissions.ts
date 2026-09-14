import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export interface DomainPermission {
    id: number;
    user_id: number;
    email: string;
    organisation: string;
    domain: string;
    allowed_subpath: string;
    can_read: boolean;
    can_write: boolean;
    can_delete: boolean;
    note?: string;
}

export interface PermissionsList {
    permissions: DomainPermission[];
    count: number;
}

export interface RevokeResult {
    success: boolean;
    revoked: string;
    warning?: string;
}

async function request<T>(t_url: string, t_init?: RequestInit): Promise<SgrnResult<T>> {
    try {
        const res = await authenticatedFetch(t_url, t_init);
        return await processResponse<T>(res);
    } catch (e) {
        return {
            error: `Permissions request failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export function fetchPermissions(t_params: { user_id?: string; email?: string; domain?: string; organisation?: string } = {}) {
    const params = new URLSearchParams();
    if (t_params.user_id?.trim()) params.set("user_id", t_params.user_id.trim());
    if (t_params.email?.trim()) params.set("email", t_params.email.trim());
    if (t_params.domain?.trim()) params.set("domain", t_params.domain.trim());
    if (t_params.organisation?.trim()) params.set("organisation", t_params.organisation.trim());
    const query = params.toString() ? `?${params.toString()}` : "";
    return request<PermissionsList>(`${AdminBackendApiEndpoints.PERMISSIONS}${query}`);
}

export function grantPermission(t_body: {
    user_id?: number;
    email?: string;
    organisation?: string;
    domain: string;
    allowed_subpath?: string;
    can_read?: boolean;
    can_write?: boolean;
    can_delete?: boolean;
}): Promise<SgrnResult<DomainPermission>> {
    return request<DomainPermission>(AdminBackendApiEndpoints.PERMISSIONS, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(t_body),
    });
}

export function revokePermission(t_by: { id: number } | { user_id: number; organisation: string; domain: string }) {
    const params = new URLSearchParams();
    if ("id" in t_by) {
        params.set("id", String(t_by.id));
    } else {
        params.set("user_id", String(t_by.user_id));
        params.set("organisation", t_by.organisation);
        params.set("domain", t_by.domain);
    }
    return request<RevokeResult>(`${AdminBackendApiEndpoints.PERMISSIONS}?${params.toString()}`, { method: "DELETE" });
}
