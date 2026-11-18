import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, jsonBody, processResponse, rawJson } from "@/backend/api/fetcher";
import type { UpdateSystemConfigRequest } from "@sgrn/datastore/requests";
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

// System config is an arbitrary JSON document keyed by dotted paths.
export type SystemConfigValues = Record<string, unknown>;

export interface SystemConfigDocument {
    config_path: string;
    values: SystemConfigValues;
    live: SystemConfigValues;
    schema: SystemConfigFieldSpec[];
    pending_restart: string[];
}

export interface SystemConfigSaveResult {
    config_path: string;
    backup_path: string;
    applied_hot: string[];
    restart_required: string[];
    pending_restart: string[];
    warnings: string[];
}

function lookupPath(t_root: unknown, t_dotted: string): unknown {
    let node: unknown = t_root;
    for (const part of t_dotted.split(".")) {
        if (node === null || typeof node !== "object" || !(part in node)) {
            return undefined;
        }
        node = (node as Record<string, unknown>)[part];
    }
    return node;
}

function assignPath(t_root: SystemConfigValues, t_dotted: string, t_value: unknown): void {
    const parts = t_dotted.split(".");
    let node = t_root;
    for (let i = 0; i < parts.length - 1; i++) {
        const key = parts[i];
        const next = node[key];
        if (next === null || typeof next !== "object") {
            node[key] = {};
        }
        node = node[key] as SystemConfigValues;
    }
    node[parts[parts.length - 1]] = t_value;
}

export function getFieldValue(t_values: SystemConfigValues, t_path: string): unknown {
    return lookupPath(t_values, t_path);
}

export function setFieldValue(t_values: SystemConfigValues, t_path: string, t_value: unknown): SystemConfigValues {
    const copy = JSON.parse(JSON.stringify(t_values)) as SystemConfigValues;
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

export async function updateSystemConfig(t_values: SystemConfigValues): Promise<SgrnResult<SystemConfigSaveResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.SYSTEM_CONFIG, {
            method: "PUT",
            ...jsonBody<UpdateSystemConfigRequest>({ values: rawJson(t_values) }),
        });
        return await processResponse<SystemConfigSaveResult>(res);
    } catch (e) {
        return {
            error: `Failed to save system config: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
