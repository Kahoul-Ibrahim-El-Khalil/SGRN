import { authenticatedFetch, processResponse } from "./fetcher";
import { AdminBackendApiEndpoints } from "../endpoints";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export interface AuditLogEntry {
    id: number;
    organisation: string;
    actor_type: "user" | "automated_service" | "system";
    actor_id: number | null;
    actor_name: string;
    action: string;
    target_type: string;
    target_id: string;
    ip: string;
    details: Record<string, unknown>;
    status: "success" | "failure";
    created_at: string;
}

export interface FetchAuditLogsParams {
    actor_type?: string;
    action?: string;
    status?: string;
    limit?: number;
    offset?: number;
}

export interface AuditLogsResult {
    success: boolean;
    audit_logs: AuditLogEntry[];
    limit: number;
    offset: number;
}

export interface PurgeAuditLogsResult {
    success: boolean;
    records_purged: number;
    older_than_days: number;
    message: string;
}

export async function fetchAuditLogs(params?: FetchAuditLogsParams): Promise<SgrnResult<AuditLogsResult>> {
    try {
        const query = new URLSearchParams();
        if (params?.actor_type) query.set("actor_type", params.actor_type);
        if (params?.action) query.set("action", params.action);
        if (params?.status) query.set("status", params.status);
        if (params?.limit) query.set("limit", String(params.limit));
        if (params?.offset) query.set("offset", String(params.offset));

        const url = `${AdminBackendApiEndpoints.AUDIT_LOGS}?${query.toString()}`;
        const res = await authenticatedFetch(url);
        return await processResponse<AuditLogsResult>(res);
    } catch (e) {
        return {
            error: `Fetch audit logs failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export async function purgeAuditLogs(older_than_days: number = 30): Promise<SgrnResult<PurgeAuditLogsResult>> {
    try {
        const url = `${AdminBackendApiEndpoints.AUDIT_LOGS}?older_than_days=${older_than_days}`;
        const res = await authenticatedFetch(url, { method: "DELETE" });
        return await processResponse<PurgeAuditLogsResult>(res);
    } catch (e) {
        return {
            error: `Purge audit logs failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
