// State and actions of the admin storage tools: bucket census, search by
// content key, and the orphan scan / purge workflow.
import { useState } from "react";
import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, expectOk, jsonBody } from "@/backend/api/fetcher";
import { useEvent } from "@/contexts/EventContext";
import { useAsyncResource } from "@/hooks/useAsyncResource";
import { useFormState } from "@/hooks/useFormState";
import { usePending } from "@/hooks/usePending";
import { errorMessage } from "@/lib/errors";
import type { JsonObject, PurgeOrphansRequest } from "@sgrn/datastore/requests";
import type { OrphansResponse, OverviewResponse } from "@sgrn/types";

type BucketStats = OverviewResponse["buckets"][number];

/** GET /admin/storage/search — where one content key lives. */
export interface KeySearchResult {
    bucket: string;
    key: string;
    db?: JsonObject | null;
    files?: JsonObject[] | null;
    /** S3 stat document for the object; null when absent or unreachable. */
    garage: JsonObject | null;
    garage_error?: string | null;
}

/** POST /admin/storage/purge-orphans — dry run or real deletion. */
export interface PurgeResult {
    bucket: string;
    dry_run: boolean;
    deleted?: string[];
    would_delete?: unknown[];
    would_delete_count?: number;
    /** Per-key delete errors reported by the S3 backend. */
    errors?: JsonObject[];
    skipped?: unknown[];
}

async function listBuckets(t_signal: AbortSignal): Promise<BucketStats[]> {
    const response = await authenticatedFetch(AdminBackendApiEndpoints.STORAGE_OVERVIEW, {
        signal: t_signal,
    });
    const payload = await expectOk<OverviewResponse>(response, "Failed to load storage census");
    return Array.isArray(payload.buckets) ? payload.buckets : [];
}

export function useStorageAdmin() {
    const { showEvent } = useEvent();

    // ── Census ──────────────────────────────────────────────────────────────
    const census = useAsyncResource<BucketStats[]>(listBuckets, [], {
        auto: false,
        onError: () => showEvent("error", "Unable to load storage census"),
    });

    // ── Search by content key ───────────────────────────────────────────────
    const searchForm = useFormState({ hash: "" });
    const [storageSearchResult, setStorageSearchResult] = useState<KeySearchResult | null>(null);
    const searching = usePending();

    const searchStorageByHash = () => {
        const hash = searchForm.values.hash.trim();
        if (!hash) {
            showEvent("error", "Enter an object hash (content key) first");
            return;
        }
        return searching.run(async () => {
            try {
                const response = await authenticatedFetch(`${AdminBackendApiEndpoints.STORAGE_SEARCH}?key=${encodeURIComponent(hash)}`);
                setStorageSearchResult(await expectOk<KeySearchResult>(response, "Storage search failed"));
            } catch (error) {
                console.error("Storage search failed:", error);
                showEvent("error", "Storage search failed");
            }
        });
    };

    // ── Orphan scan / purge ─────────────────────────────────────────────────
    const orphanForm = useFormState({ bucket: "", prefix: "" });
    const [orphanReport, setOrphanReport] = useState<OrphansResponse | null>(null);
    const [purgeResult, setPurgeResult] = useState<PurgeResult | null>(null);
    const scanning = usePending();
    const purging = usePending();

    const scanStorageOrphans = () =>
        scanning.run(async () => {
            setPurgeResult(null);
            try {
                const params = new URLSearchParams();
                if (orphanForm.values.bucket.trim()) params.set("bucket", orphanForm.values.bucket.trim());
                if (orphanForm.values.prefix.trim()) params.set("prefix", orphanForm.values.prefix.trim());
                const query = params.toString() ? `?${params.toString()}` : "";
                const response = await authenticatedFetch(`${AdminBackendApiEndpoints.STORAGE_ORPHANS}${query}`, {});
                setOrphanReport(await expectOk<OrphansResponse>(response, "Orphan scan failed"));
            } catch (error) {
                console.error("Orphan scan failed:", error);
                showEvent("error", "Orphan scan failed");
            }
        });

    const purgeStorageOrphans = (t_dryRun: boolean) => {
        if (!orphanReport) return;
        return purging.run(async () => {
            try {
                const response = await authenticatedFetch(AdminBackendApiEndpoints.STORAGE_PURGE_ORPHANS, {
                    method: "POST",
                    ...jsonBody<PurgeOrphansRequest>({
                        bucket: orphanReport.bucket,
                        prefix: orphanReport.prefix ?? "",
                        dry_run: t_dryRun,
                    }),
                });
                const payload = await expectOk<PurgeResult>(response, "Purge failed");
                setPurgeResult(payload);
                if (!t_dryRun) {
                    showEvent("success", `Purged ${payload.deleted?.length ?? 0} orphan key(s)`);
                    void scanStorageOrphans();
                }
            } catch (error) {
                console.error("Purge failed:", error);
                showEvent("error", errorMessage(error, "Orphan purge failed"));
            }
        });
    };

    return {
        storageBuckets: census.data,
        loadingStorageOverview: census.loading,
        fetchStorageOverview: census.reload,

        storageSearchInput: searchForm.values.hash,
        setStorageSearchInput: (t_hash: string) => searchForm.setField("hash", t_hash),
        storageSearchResult,
        searchingStorage: searching.pending,
        searchStorageByHash,

        orphanBucket: orphanForm.values.bucket,
        setOrphanBucket: (t_bucket: string) => orphanForm.setField("bucket", t_bucket),
        orphanPrefix: orphanForm.values.prefix,
        setOrphanPrefix: (t_prefix: string) => orphanForm.setField("prefix", t_prefix),
        orphanReport,
        purgeResult,
        scanningOrphans: scanning.pending,
        purgingOrphans: purging.pending,
        scanStorageOrphans,
        purgeStorageOrphans,
    };
}
