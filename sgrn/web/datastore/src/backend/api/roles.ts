import { authenticatedFetch, processResponse } from "./fetcher";
import { AdminBackendApiEndpoints } from "../endpoints";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export interface RoleEntry {
    id: number;
    name: string;
    description: string;
    permissions: string[];
    is_system: boolean;
    created_at: string;
    updated_at: string;
}

export interface RolesListResult {
    success: boolean;
    roles: RoleEntry[];
}

export interface GenericRoleResult {
    success: boolean;
    message: string;
    role_id?: number;
}

export async function fetchRoles(): Promise<SgrnResult<RolesListResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.ROLES);
        return await processResponse<RolesListResult>(res);
    } catch (e) {
        return {
            error: `Fetch roles failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function createRole(role: {
    name: string;
    description?: string;
    permissions?: string[];
}): Promise<SgrnResult<GenericRoleResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.ROLES, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(role),
        });
        return await processResponse<GenericRoleResult>(res);
    } catch (e) {
        return {
            error: `Create role failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function updateRole(
    id: number,
    role: { description?: string; permissions?: string[] },
): Promise<SgrnResult<GenericRoleResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.ROLES_ITEM(id), {
            method: "PUT",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(role),
        });
        return await processResponse<GenericRoleResult>(res);
    } catch (e) {
        return {
            error: `Update role failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function deleteRole(id: number): Promise<SgrnResult<GenericRoleResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.ROLES_ITEM(id), {
            method: "DELETE",
        });
        return await processResponse<GenericRoleResult>(res);
    } catch (e) {
        return {
            error: `Delete role failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function assignRole(payload: {
    role_id: number;
    user_id?: number;
    automated_service_id?: number;
}): Promise<SgrnResult<GenericRoleResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.ROLES_ASSIGN, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload),
        });
        return await processResponse<GenericRoleResult>(res);
    } catch (e) {
        return {
            error: `Assign role failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function revokeRole(payload: {
    role_id: number;
    user_id?: number;
    automated_service_id?: number;
}): Promise<SgrnResult<GenericRoleResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.ROLES_REVOKE, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload),
        });
        return await processResponse<GenericRoleResult>(res);
    } catch (e) {
        return {
            error: `Revoke role failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
