import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export interface AnalyticsCounts {
    users: number;
    automated_services: number;
    organisations: number;
    domains: number;
    files: number;
    objects: number;
    formats: number;
    permission_grants: number;
}

export interface TimeseriesPoint {
    day: string;
    files: number;
    bytes_virtual: number;
}

export interface AnalyticsOverview {
    counts: AnalyticsCounts;
    bytes: { virtual: number; stored: number; saved_by_compression: number };
    timeseries_30d: TimeseriesPoint[];
    top_extensions: { extension: string; files: number; bytes_virtual: number }[];
    top_uploaders: { actor: string; files: number; bytes_virtual: number }[];
    recent_files: { name: string; extension: string; bytes_virtual: number; actor: string; at: string }[];
}

export type BreakdownKind = "domain" | "organisation" | "user" | "service";

export interface BreakdownRow {
    slice: string;
    files: number;
    bytes_virtual: number;
    actors?: number;
    storage_limit_bytes?: number | null;
    entry_count_limit?: number | null;
}

export interface AnalyticsBreakdown {
    kind: BreakdownKind;
    rows: BreakdownRow[];
    count: number;
}

async function request<T>(t_url: string): Promise<SgrnResult<T>> {
    try {
        const res = await authenticatedFetch(t_url);
        return await processResponse<T>(res);
    } catch (e) {
        return {
            error: `Analytics request failed: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}

export function fetchAnalyticsOverview(): Promise<SgrnResult<AnalyticsOverview>> {
    return request<AnalyticsOverview>(AdminBackendApiEndpoints.ANALYTICS_OVERVIEW);
}

export function fetchAnalyticsBreakdown(t_kind: BreakdownKind, t_search = "", t_limit = 100): Promise<SgrnResult<AnalyticsBreakdown>> {
    const params = new URLSearchParams({ kind: t_kind, limit: String(t_limit) });
    if (t_search.trim()) params.set("search", t_search.trim());
    return request<AnalyticsBreakdown>(`${AdminBackendApiEndpoints.ANALYTICS_BREAKDOWN}?${params.toString()}`);
}

export function formatBytes(t_bytes: number): string {
    if (t_bytes <= 0) return "0 B";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let v = t_bytes;
    let u = 0;
    while (v >= 1024 && u < units.length - 1) {
        v /= 1024;
        u++;
    }
    return `${v >= 100 ? Math.round(v) : Math.round(v * 10) / 10} ${units[u]}`;
}
