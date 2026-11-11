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

export async function fetchAuditLogs(t_params?: FetchAuditLogsParams): Promise<SgrnResult<AuditLogsResult>> {
    try {
        const query = new URLSearchParams();
        if (t_params?.actor_type) query.set("actor_type", t_params.actor_type);
        if (t_params?.action) query.set("action", t_params.action);
        if (t_params?.status) query.set("status", t_params.status);
        if (t_params?.limit) query.set("limit", String(t_params.limit));
        if (t_params?.offset) query.set("offset", String(t_params.offset));

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

export async function purgeAuditLogs(t_older_than_days: number = 30): Promise<SgrnResult<PurgeAuditLogsResult>> {
    try {
        const url: string = `${AdminBackendApiEndpoints.AUDIT_LOGS}?older_than_days=${t_older_than_days}`;
        const res = await authenticatedFetch(url, { method: "DELETE" });
        return await processResponse<PurgeAuditLogsResult>(res);
    } catch (e) {
        return {
            error: `Purge audit logs failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export interface VerifyAuditLogsResult {
    is_valid: boolean;
    total_rows: number;
    verified_checkpoints: number;
    status: "verified" | "tampered";
    message: string;
    first_broken_seq?: number;
    first_broken_id?: number;
}

export async function verifyAuditLogs(): Promise<SgrnResult<VerifyAuditLogsResult>> {
    try {
        const res = await authenticatedFetch(AdminBackendApiEndpoints.AUDIT_VERIFY);
        return await processResponse<VerifyAuditLogsResult>(res);
    } catch (e) {
        return {
            error: `Audit verification failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
