import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export interface StorageFormat {
    extension: string;
    mime_type: string;
    is_compressed: boolean;
    is_allowed: boolean;
    description: string | null;
    created_at: string;
    created?: boolean;
}

export interface FormatsList {
    formats: StorageFormat[];
    count: number;
}

export type QuotaKind = "user" | "service" | "organisation" | "domain";

export interface QuotaRow {
    id?: number;
    email?: string;
    name?: string;
    label?: string;
    organisation: string | null;
    storage_used_bytes: number;
    storage_limit_bytes: number | null;
    entries_used: number;
    entry_count_limit: number | null;
    max_file_size_mb?: number | null;
    preferred_chunk_size_mb?: number | null;
    rate_limit_upload_rpm?: number | null;
    rate_limit_rpm?: number | null;
}

export interface QuotasList {
    kind: QuotaKind;
    rows: QuotaRow[];
    count: number;
}

async function request<T>(t_url: string, t_init?: RequestInit): Promise<SgrnResult<T>> {
    try {
        const res = await authenticatedFetch(t_url, t_init);
        return await processResponse<T>(res);
    } catch (e) {
        return {
            error: `Governance request failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export function fetchFormats(t_search = "", t_allowed = "", t_limit = 500): Promise<SgrnResult<FormatsList>> {
    const params = new URLSearchParams();
    if (t_search.trim()) params.set("search", t_search.trim());
    if (t_allowed === "true" || t_allowed === "false") params.set("allowed", t_allowed);
    params.set("limit", String(t_limit));
    const query = params.toString() ? `?${params.toString()}` : "";
    return request<FormatsList>(`${AdminBackendApiEndpoints.STORAGE_FORMATS}${query}`);
}

export function upsertFormat(t_body: {
    extension: string;
    mime_type: string;
    is_compressed?: boolean;
    is_allowed?: boolean;
    description?: string;
}): Promise<SgrnResult<StorageFormat>> {
    return request<StorageFormat>(AdminBackendApiEndpoints.STORAGE_FORMATS, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(t_body),
    });
}

export function deleteFormat(t_extension: string): Promise<SgrnResult<{ success: boolean; extension: string }>> {
    return request<{ success: boolean; extension: string }>(
        `${AdminBackendApiEndpoints.STORAGE_FORMATS}?extension=${encodeURIComponent(t_extension)}`,
        { method: "DELETE" },
    );
}

// Unified response of GET /api/v1/admin/quotas (StorageAdminHandler::handleListQuotas):
// { kind, rows: [...], count }. Rows are kind-shaped but always carry the
// usage/limit pair; extra columns appear per kind (path for domains,
// rate limits for users/services/organisations).
export interface BackendQuotaRow {
    kind: QuotaKind;
    id?: number;
    email?: string;
    name?: string;
    path?: string;
    organisation?: string | null;
    storage_used_bytes: number;
    storage_limit_bytes: number | null;
    entries_used: number;
    entry_count_limit: number | null;
    rate_limit_rpm?: number | null;
    max_file_size_mb?: number | null;
    preferred_chunk_size_mb?: number | null;
    rate_limit_upload_rpm?: number | null;
}

export interface QuotaRow {
    id?: number;
    email?: string;
    name?: string;
    label?: string;
    path?: string;
    organisation: string | null;
    storage_used_bytes: number;
    storage_limit_bytes: number | null;
    entries_used: number;
    entry_count_limit: number | null;
    max_file_size_mb?: number | null;
    preferred_chunk_size_mb?: number | null;
    rate_limit_upload_rpm?: number | null;
    rate_limit_rpm?: number | null;
}

export async function fetchQuotas(t_kind: QuotaKind, t_search = "", t_limit = 100): Promise<SgrnResult<QuotasList>> {
    const quotaParams = new URLSearchParams({
        kind: t_kind,
        limit: String(t_limit),
    });
    if (t_search.trim()) {
        quotaParams.set("search", t_search.trim());
    }
    const rawRes = await request<{ kind: QuotaKind; rows: BackendQuotaRow[]; count: number }>(
        `${AdminBackendApiEndpoints.QUOTAS}?${quotaParams.toString()}`,
    );
    if (rawRes.error || !rawRes.data) {
        return { error: rawRes.error || "Failed to load quotas", scope: rawRes.scope };
    }

    // The backend returns a unified row shape for every kind (usage,
    // limits, and per-kind extras like `path` for domains). Pass the
    // rows straight through — QuotaRow is structurally compatible.
    const rows: QuotaRow[] = rawRes.data.rows.map((r) => ({
        id: r.id,
        email: r.email,
        name: r.name,
        path: r.path,
        organisation: r.organisation ?? null,
        storage_used_bytes: r.storage_used_bytes ?? 0,
        storage_limit_bytes: r.storage_limit_bytes ?? null,
        entries_used: r.entries_used ?? 0,
        entry_count_limit: r.entry_count_limit ?? null,
        max_file_size_mb: r.max_file_size_mb ?? null,
        preferred_chunk_size_mb: r.preferred_chunk_size_mb ?? null,
        rate_limit_upload_rpm: r.rate_limit_upload_rpm ?? null,
        rate_limit_rpm: r.rate_limit_rpm ?? null,
    }));

    return {
        data: {
            kind: t_kind,
            rows,
            count: rows.length,
        },
    };
}

export function updateQuota(t_body: {
    kind: QuotaKind;
    id?: number;
    name?: string;
    storage_limit_bytes?: number | null;
    entry_count_limit?: number | null;
    max_file_size_mb?: number | null;
    preferred_chunk_size_mb?: number | null;
    rate_limit_upload_rpm?: number | null;
    rate_limit_rpm?: number | null;
}): Promise<SgrnResult<QuotaRow & { kind: QuotaKind }>> {
    let endpoint: string = AdminBackendApiEndpoints.QUOTAS;
    if (t_body.kind === "user" && t_body.id !== undefined) {
        endpoint = AdminBackendApiEndpoints.QUOTAS_USER(t_body.id);
    } else if (t_body.kind === "service" && t_body.id !== undefined) {
        endpoint = AdminBackendApiEndpoints.QUOTAS_SERVICE(t_body.id);
    } else if (t_body.kind === "organisation") {
        endpoint = AdminBackendApiEndpoints.QUOTAS_ORG;
        // Platform admins may target another organisation via ?name=;
        // tenant sessions are scoped server-side to their own row.
        if (t_body.name) {
            endpoint += `?name=${encodeURIComponent(t_body.name)}`;
        }
    } else if (t_body.kind === "domain") {
        endpoint = AdminBackendApiEndpoints.QUOTAS_DOMAIN(t_body.id!);
    }

    const payload = {
        storage_limit: t_body.storage_limit_bytes,
        entry_count_limit: t_body.entry_count_limit,
        max_file_size_mb: t_body.max_file_size_mb,
        preferred_chunk_size_mb: t_body.preferred_chunk_size_mb,
        rate_limit_upload_rpm: t_body.rate_limit_upload_rpm,
        rate_limit_rpm: t_body.rate_limit_rpm,
    };

    return request<QuotaRow & { kind: QuotaKind }>(endpoint, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
    });
}

export const BYTES_PER_MB = 1024 * 1024;

export function bytesToMb(t_bytes: number | null): string {
    if (t_bytes === null || t_bytes === undefined) return "";
    return String(Math.floor((t_bytes / BYTES_PER_MB) * 100) / 100);
}

export function mbToBytes(t_mb: string): number | null {
    const trimmed = t_mb.trim();
    if (trimmed === "") return null;
    const v = Number(trimmed);
    if (!Number.isFinite(v) || v < 0) return null;
    return Math.floor(v * BYTES_PER_MB);
}
