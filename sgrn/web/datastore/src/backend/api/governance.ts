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

export type QuotaKind = "user" | "service" | "organisation";

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

interface BackendGetQuotasResp {
    success: boolean;
    organisation?: {
        total_virtual_size: number;
        total_real_size: number;
        storage_limit: number | null;
        total_entry_count: number;
        entry_count_limit: number | null;
        rate_limit_rpm: number | null;
    };
    users?: Array<{
        id: number;
        email: string;
        total_virtual_size: number;
        total_real_size: number;
        storage_limit: number | null;
        total_entry_count: number;
        entry_count_limit: number | null;
        rate_limit_rpm: number | null;
        max_file_size_mb?: number | null;
        preferred_chunk_size_mb?: number | null;
        rate_limit_upload_rpm?: number | null;
    }>;
    automated_services?: Array<{
        id: number;
        name: string;
        token: string;
        total_virtual_size: number;
        total_real_size: number;
        storage_limit: number | null;
        total_entry_count: number;
        entry_count_limit: number | null;
        rate_limit_rpm: number | null;
        max_file_size_mb?: number | null;
        preferred_chunk_size_mb?: number | null;
        rate_limit_upload_rpm?: number | null;
    }>;
}

export async function fetchQuotas(t_kind: QuotaKind, t_search = "", t_limit = 100): Promise<SgrnResult<QuotasList>> {
    const quotaParams = new URLSearchParams({ limit: String(t_limit) });
    const rawRes = await request<BackendGetQuotasResp>(`${AdminBackendApiEndpoints.QUOTAS}?${quotaParams.toString()}`);
    if (rawRes.error || !rawRes.data) {
        return { error: rawRes.error || "Failed to load quotas", scope: rawRes.scope };
    }

    const data = rawRes.data;
    const searchLower = t_search.trim().toLowerCase();
    let rows: QuotaRow[] = [];

    if (t_kind === "user" && data.users) {
        rows = data.users
            .filter((u) => !searchLower || u.email.toLowerCase().includes(searchLower))
            .map((u) => ({
                id: u.id,
                email: u.email,
                organisation: null,
                storage_used_bytes: u.total_real_size ?? 0,
                storage_limit_bytes: u.storage_limit ?? null,
                entries_used: u.total_entry_count ?? 0,
                entry_count_limit: u.entry_count_limit ?? null,
                max_file_size_mb: u.max_file_size_mb ?? null,
                preferred_chunk_size_mb: u.preferred_chunk_size_mb ?? null,
                rate_limit_upload_rpm: u.rate_limit_upload_rpm ?? null,
                rate_limit_rpm: u.rate_limit_rpm ?? null,
            }));
    } else if (t_kind === "service" && data.automated_services) {
        rows = data.automated_services
            .filter((s) => !searchLower || s.name.toLowerCase().includes(searchLower))
            .map((s) => ({
                id: s.id,
                name: s.name,
                organisation: null,
                storage_used_bytes: s.total_real_size ?? 0,
                storage_limit_bytes: s.storage_limit ?? null,
                entries_used: s.total_entry_count ?? 0,
                entry_count_limit: s.entry_count_limit ?? null,
                max_file_size_mb: s.max_file_size_mb ?? null,
                preferred_chunk_size_mb: s.preferred_chunk_size_mb ?? null,
                rate_limit_upload_rpm: s.rate_limit_upload_rpm ?? null,
                rate_limit_rpm: s.rate_limit_rpm ?? null,
            }));
    } else if (t_kind === "organisation" && data.organisation) {
        const o = data.organisation;
        rows = [
            {
                name: "Primary Organisation",
                organisation: null,
                storage_used_bytes: o.total_real_size ?? 0,
                storage_limit_bytes: o.storage_limit ?? null,
                entries_used: o.total_entry_count ?? 0,
                entry_count_limit: o.entry_count_limit ?? null,
                rate_limit_rpm: o.rate_limit_rpm ?? null,
            },
        ];
    }

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
