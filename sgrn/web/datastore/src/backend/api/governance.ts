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

export function fetchQuotas(t_kind: QuotaKind, t_search = "", t_limit = 100): Promise<SgrnResult<QuotasList>> {
    const params = new URLSearchParams({ kind: t_kind, limit: String(t_limit) });
    if (t_search.trim()) params.set("search", t_search.trim());
    return request<QuotasList>(`${AdminBackendApiEndpoints.QUOTAS}?${params.toString()}`);
}

export function updateQuota(t_body: {
    kind: QuotaKind;
    id?: number;
    name?: string;
    storage_limit_bytes?: number | null;
    entry_count_limit?: number | null;
}): Promise<SgrnResult<QuotaRow & { kind: QuotaKind }>> {
    return request<QuotaRow & { kind: QuotaKind }>(AdminBackendApiEndpoints.QUOTAS, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(t_body),
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
