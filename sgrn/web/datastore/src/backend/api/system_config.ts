import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export type SystemConfigFieldType = "uint" | "double" | "string" | "bool" | "string[]";

export interface SystemConfigFieldSpec {
    path: string;
    type: SystemConfigFieldType;
    hot: boolean;
    section: string;
    label: string;
    min?: number;
    max?: number;
}

export interface SystemConfigDocument {
    config_path: string;
    values: Record<string, any>;
    live: Record<string, any>;
    schema: SystemConfigFieldSpec[];
    pending_restart: string[];
}

export interface SystemConfigSaveResult {
    success: boolean;
    config_path: string;
    backup_path: string;
    applied_hot: string[];
    restart_required: string[];
    pending_restart: string[];
    warnings: string[];
}

function lookupPath(t_root: any, t_dotted: string): any {
    let node = t_root;
    for (const part of t_dotted.split(".")) {
        if (node === null || typeof node !== "object" || !(part in node)) {
            return undefined;
        }
        node = node[part];
    }
    return node;
}

function assignPath(t_root: any, t_dotted: string, t_value: any): void {
    const parts = t_dotted.split(".");
    let node = t_root;
    for (let i = 0; i < parts.length - 1; i++) {
        if (node[parts[i]] === null || typeof node[parts[i]] !== "object") {
            node[parts[i]] = {};
        }
        node = node[parts[i]];
    }
    node[parts[parts.length - 1]] = t_value;
}

export function getFieldValue(t_values: Record<string, any>, t_path: string): any {
    return lookupPath(t_values, t_path);
}

export function setFieldValue(t_values: Record<string, any>, t_path: string, t_value: any): Record<string, any> {
    const copy = JSON.parse(JSON.stringify(t_values));
    assignPath(copy, t_path, t_value);
    return copy;
}

export async function fetchSystemConfig(): Promise<SgrnResult<SystemConfigDocument>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.SYSTEM_CONFIG);
        return await processResponse<SystemConfigDocument>(res);
    } catch (e) {
        return {
            error: `Failed to fetch system config: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function updateSystemConfig(t_values: Record<string, any>): Promise<SgrnResult<SystemConfigSaveResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.SYSTEM_CONFIG, {
            method: "PUT",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ values: t_values }),
        });
        return await processResponse<SystemConfigSaveResult>(res);
    } catch (e) {
        return {
            error: `Failed to save system config: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
