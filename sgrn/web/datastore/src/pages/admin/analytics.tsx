import { useState, useCallback, useEffect } from "react";
import { Loader2, Search, RefreshCw, Trash2, ShieldAlert, Activity, ShieldCheck } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import {
    fetchAnalyticsOverview,
    fetchAnalyticsBreakdown,
    formatBytes,
    type AnalyticsOverview,
    type BreakdownKind,
    type BreakdownRow,
} from "@/backend/api/analytics";
import { fetchAuditLogs, purgeAuditLogs, verifyAuditLogs, type AuditLogEntry, type VerifyAuditLogsResult } from "@/backend/api/audit";

function StatCell({ t_label, t_value, t_sub }: { t_label: string; t_value: string; t_sub?: string }) {
    return (
        <div className="stat-tile">
            <div className="stat-tile-label">{t_label}</div>
            <div className="stat-tile-value">{t_value}</div>
            <div className="stat-tile-sub">{t_sub ?? "\u00a0"}</div>
        </div>
    );
}

export default function AnalyticsPanel() {
    const { showEvent } = useEvent();
    const [overview, setOverview] = useState<AnalyticsOverview | null>(null);
    const [loading, setLoading] = useState(false);
    const [breakdownKind, setBreakdownKind] = useState<BreakdownKind>("domain");
    const [breakdownSearch, setBreakdownSearch] = useState("");
    const [breakdownRows, setBreakdownRows] = useState<BreakdownRow[]>([]);
    const [loadingBreakdown, setLoadingBreakdown] = useState(false);

    // Audit State
    const [auditLogs, setAuditLogs] = useState<AuditLogEntry[]>([]);
    const [loadingAudit, setLoadingAudit] = useState(false);
    const [auditActionFilter, setAuditActionFilter] = useState("");
    const [purgeDays, setPurgeDays] = useState(30);
    const [purging, setPurging] = useState(false);
    const [verifying, setVerifying] = useState(false);
    const [verifyResult, setVerifyResult] = useState<VerifyAuditLogsResult | null>(null);

    const loadOverview = useCallback(async () => {
        setLoading(true);
        try {
            const result = await fetchAnalyticsOverview();
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load analytics");
            }
            setOverview(result.data);
        } catch (error) {
            console.error("Failed to fetch analytics:", error);
            showEvent("error", "Unable to load analytics overview");
        } finally {
            setLoading(false);
        }
    }, [showEvent]);

    const loadBreakdown = useCallback(async () => {
        setLoadingBreakdown(true);
        try {
            const result = await fetchAnalyticsBreakdown(breakdownKind, breakdownSearch);
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load breakdown");
            }
            setBreakdownRows(result.data.rows);
        } catch (error) {
            console.error("Failed to fetch breakdown:", error);
            showEvent("error", "Unable to load breakdown");
        } finally {
            setLoadingBreakdown(false);
        }
    }, [breakdownKind, breakdownSearch, showEvent]);

    const loadAudit = useCallback(async () => {
        setLoadingAudit(true);
        try {
            const result = await fetchAuditLogs({ action: auditActionFilter, limit: 50 });
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load audit logs");
            }
            setAuditLogs(result.data.audit_logs);
        } catch (error) {
            console.error("Failed to fetch audit logs:", error);
            showEvent("error", "Unable to load audit logs");
        } finally {
            setLoadingAudit(false);
        }
    }, [auditActionFilter, showEvent]);

    const handlePurgeAudit = useCallback(async () => {
        setPurging(true);
        try {
            const result = await purgeAuditLogs(purgeDays);
            if (result.error || !result.data) {
                throw new Error(result.error || "Purge failed");
            }
            showEvent(
                "success",
                `Purged ${result.data.records_purged} audit records older than ${result.data.older_than_days} days. Logged 'audit.purged' event.`,
            );
            await loadAudit();
        } catch (error) {
            console.error("Audit purge failed:", error);
            showEvent("error", error instanceof Error ? error.message : "Audit purge failed");
        } finally {
            setPurging(false);
        }
    }, [purgeDays, loadAudit, showEvent]);

    const handleVerifyAudit = useCallback(async () => {
        setVerifying(true);
        try {
            const result = await verifyAuditLogs();
            // The backend returns 409 when the chain is tampered — still a
            // well-formed VerifyAuditLogsResult body, so both outcomes flow
            // through result.data.
            if (!result.data) {
                throw new Error(result.error || "Verification failed");
            }
            setVerifyResult(result.data);
            if (result.data.is_valid) {
                showEvent(
                    "success",
                    `Audit chain verified: ${result.data.total_rows} rows, ${result.data.verified_checkpoints} checkpoints intact.`,
                );
            } else {
                showEvent(
                    "error",
                    `AUDIT CHAIN TAMPERED — first break at seq ${result.data.first_broken_seq ?? "?"} (row ${result.data.first_broken_id ?? "?"}): ${result.data.message}`,
                );
            }
        } catch (error) {
            console.error("Audit verification failed:", error);
            showEvent("error", error instanceof Error ? error.message : "Audit verification failed");
        } finally {
            setVerifying(false);
        }
    }, [showEvent]);

    useEffect(() => {
        loadOverview();
        loadBreakdown();
        loadAudit();
    }, [loadOverview, loadBreakdown, loadAudit]);

    const maxDayFiles = Math.max(1, ...(overview?.timeseries_30d.map((p) => p.files) ?? [1]));

    return (
        <div className="query-builder">
            <div className="query-controls">
                <div className="query-controls-row">
                    <button className="btn-desktop-primary" onClick={loadOverview} disabled={loading}>
                        {loading ? <Loader2 className="animate-spin" size={18} /> : <RefreshCw size={18} />}
                        <span>&nbsp;LOAD ANALYTICS</span>
                    </button>
                </div>
            </div>

            {overview && (
                <>
                    <div className="stat-grid">
                        <StatCell t_label="FILES" t_value={String(overview.counts.files)} t_sub={`${overview.counts.objects} objects`} />
                        <StatCell t_label="VIRTUAL BYTES" t_value={formatBytes(overview.bytes.virtual)} />
                        <StatCell
                            t_label="STORED BYTES"
                            t_value={formatBytes(overview.bytes.stored)}
                            t_sub={`${formatBytes(overview.bytes.saved_by_compression)} saved`}
                        />
                        <StatCell
                            t_label="USERS"
                            t_value={String(overview.counts.users)}
                            t_sub={`${overview.counts.automated_services} services`}
                        />
                        <StatCell
                            t_label="ORGANISATIONS"
                            t_value={String(overview.counts.organisations)}
                            t_sub={`${overview.counts.domains} domains`}
                        />
                        <StatCell
                            t_label="GRANTS"
                            t_value={String(overview.counts.permission_grants)}
                            t_sub={`${overview.counts.formats} formats`}
                        />
                    </div>

                    {overview.timeseries_30d.length > 0 && (
                        <div className="roster-card">
                            <div className="roster-title">UPLOADS — LAST 30 DAYS</div>
                            <div className="bar-chart">
                                {overview.timeseries_30d.map((p) => (
                                    <div key={p.day} className="bar-row">
                                        <span className="bar-label">{p.day}</span>
                                        <div className="bar-track">
                                            <div className="bar-fill" style={{ width: `${Math.round((p.files / maxDayFiles) * 100)}%` }} />
                                        </div>
                                        <span className="bar-value">
                                            {p.files} · {formatBytes(p.bytes_virtual)}
                                        </span>
                                    </div>
                                ))}
                            </div>
                        </div>
                    )}

                    <div className="query-controls-row">
                        {overview.top_extensions.length > 0 && (
                            <div className="roster-card flex-1">
                                <div className="roster-title">TOP EXTENSIONS</div>
                                <div className="datagrid-wrapper">
                                    <table className="datagrid-industrial">
                                        <thead>
                                            <tr>
                                                <th>EXT</th>
                                                <th>FILES</th>
                                                <th>BYTES</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {overview.top_extensions.map((e) => (
                                                <tr key={e.extension}>
                                                    <td className="admin-cell-primary">{e.extension}</td>
                                                    <td>{e.files}</td>
                                                    <td>{formatBytes(e.bytes_virtual)}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                            </div>
                        )}
                        {overview.top_uploaders.length > 0 && (
                            <div className="roster-card flex-1">
                                <div className="roster-title">TOP UPLOADERS</div>
                                <div className="datagrid-wrapper">
                                    <table className="datagrid-industrial">
                                        <thead>
                                            <tr>
                                                <th>ACTOR</th>
                                                <th>FILES</th>
                                                <th>BYTES</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {overview.top_uploaders.map((e) => (
                                                <tr key={e.actor}>
                                                    <td className="admin-cell-primary">{e.actor}</td>
                                                    <td>{e.files}</td>
                                                    <td>{formatBytes(e.bytes_virtual)}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                            </div>
                        )}
                    </div>

                    {overview.recent_files.length > 0 && (
                        <div className="roster-card">
                            <div className="roster-title">MOST RECENT FILES</div>
                            <div className="datagrid-wrapper">
                                <table className="datagrid-industrial">
                                    <thead>
                                        <tr>
                                            <th>NAME</th>
                                            <th>ACTOR</th>
                                            <th>BYTES</th>
                                            <th>AT</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        {overview.recent_files.map((f, i) => (
                                            <tr key={`${f.name}-${i}`}>
                                                <td className="admin-cell-primary">{f.name}</td>
                                                <td>{f.actor}</td>
                                                <td>{formatBytes(f.bytes_virtual)}</td>
                                                <td className="admin-cell-muted">{f.at}</td>
                                            </tr>
                                        ))}
                                    </tbody>
                                </table>
                            </div>
                        </div>
                    )}
                </>
            )}

            <div className="roster-card">
                <div className="roster-title">BREAKDOWN LEDGER</div>
                <div className="query-controls-row">
                    <select
                        className="input-desktop w-64"
                        value={breakdownKind}
                        onChange={(e) => setBreakdownKind(e.target.value as BreakdownKind)}
                    >
                        <option value="domain">BY DOMAIN</option>
                        <option value="organisation">BY ORGANISATION</option>
                        <option value="user">BY USER</option>
                        <option value="service">BY SERVICE</option>
                    </select>
                    <input
                        className="input-desktop flex-1"
                        placeholder="SEARCH SLICE"
                        value={breakdownSearch}
                        onChange={(e) => setBreakdownSearch(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === "Enter") loadBreakdown();
                        }}
                    />
                    <button className="btn-desktop-primary w-12" onClick={loadBreakdown} disabled={loadingBreakdown}>
                        {loadingBreakdown ? <Loader2 className="animate-spin" size={18} /> : <Search size={18} />}
                    </button>
                </div>
                {breakdownRows.length > 0 && (
                    <div className="datagrid-wrapper">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>{breakdownKind.toUpperCase()}</th>
                                    <th>FILES</th>
                                    <th>BYTES</th>
                                    <th>{breakdownKind === "domain" || breakdownKind === "organisation" ? "ACTORS" : "CAPS"}</th>
                                </tr>
                            </thead>
                            <tbody>
                                {breakdownRows.map((r) => (
                                    <tr key={r.slice}>
                                        <td className="admin-cell-primary">{r.slice}</td>
                                        <td>{r.files}</td>
                                        <td>{formatBytes(r.bytes_virtual)}</td>
                                        <td>
                                            {r.actors !== undefined
                                                ? r.actors
                                                : `${r.storage_limit_bytes === null ? "∞" : formatBytes(r.storage_limit_bytes ?? 0)} / ${r.entry_count_limit === null ? "∞" : r.entry_count_limit}`}
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                )}
            </div>

            <div className="roster-card">
                <div className="roster-title flex items-center justify-between">
                    <span className="flex items-center gap-2">
                        <Activity size={16} /> PLATFORM AUDIT TRAIL
                    </span>
                    <button
                        className="btn-desktop-secondary text-xs px-2 py-1 flex items-center gap-1"
                        onClick={loadAudit}
                        disabled={loadingAudit}
                    >
                        <RefreshCw size={12} className={loadingAudit ? "animate-spin" : ""} /> REFRESH AUDIT
                    </button>
                </div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop flex-1"
                        placeholder="FILTER BY ACTION (e.g. auth.login, user.created, audit.purged)"
                        value={auditActionFilter}
                        onChange={(e) => setAuditActionFilter(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === "Enter") loadAudit();
                        }}
                    />
                    <button className="btn-desktop-primary" onClick={loadAudit} disabled={loadingAudit}>
                        {loadingAudit ? <Loader2 className="animate-spin" size={18} /> : <Search size={18} />}
                        <span>&nbsp;FILTER</span>
                    </button>
                </div>

                {auditLogs.length > 0 ? (
                    <div className="datagrid-wrapper" style={{ marginTop: "1rem" }}>
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>TIMESTAMP</th>
                                    <th>ACTION</th>
                                    <th>ACTOR</th>
                                    <th>TARGET</th>
                                    <th>IP</th>
                                    <th>STATUS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {auditLogs.map((log) => (
                                    <tr key={log.id}>
                                        <td className="admin-cell-muted" style={{ fontSize: "0.8rem" }}>
                                            {log.created_at}
                                        </td>
                                        <td>
                                            <span
                                                style={{
                                                    padding: "2px 6px",
                                                    borderRadius: "4px",
                                                    fontSize: "0.75rem",
                                                    fontFamily: "monospace",
                                                    fontWeight: 600,
                                                    backgroundColor: log.action.startsWith("audit.")
                                                        ? "rgba(245, 158, 11, 0.15)"
                                                        : "rgba(59, 130, 246, 0.15)",
                                                    color: log.action.startsWith("audit.") ? "#fbbf24" : "#60a5fa",
                                                    border: `1px solid ${log.action.startsWith("audit.") ? "rgba(245, 158, 11, 0.3)" : "rgba(59, 130, 246, 0.3)"}`,
                                                }}
                                            >
                                                {log.action}
                                            </span>
                                        </td>
                                        <td>
                                            <div className="admin-cell-bold">{log.actor_name}</div>
                                            <div className="admin-cell-muted" style={{ fontSize: "0.75rem" }}>
                                                {log.actor_type}
                                            </div>
                                        </td>
                                        <td>
                                            {log.target_type ? (
                                                <code>
                                                    {log.target_type}:{log.target_id}
                                                </code>
                                            ) : (
                                                "-"
                                            )}
                                        </td>
                                        <td className="admin-cell-muted" style={{ fontSize: "0.8rem" }}>
                                            {log.ip}
                                        </td>
                                        <td>
                                            <span
                                                style={{
                                                    padding: "2px 8px",
                                                    borderRadius: "4px",
                                                    fontSize: "0.7rem",
                                                    fontWeight: 700,
                                                    backgroundColor:
                                                        log.status === "success" ? "rgba(16, 185, 129, 0.15)" : "rgba(239, 68, 68, 0.15)",
                                                    color: log.status === "success" ? "#10b981" : "#ef4444",
                                                    border: `1px solid ${log.status === "success" ? "rgba(16, 185, 129, 0.3)" : "rgba(239, 68, 68, 0.3)"}`,
                                                }}
                                            >
                                                {log.status.toUpperCase()}
                                            </span>
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                ) : (
                    <div className="query-help" style={{ marginTop: "1rem" }}>
                        No audit records matched the filter criteria.
                    </div>
                )}
            </div>

            <div
                className="roster-card"
                style={{ marginTop: "2rem", border: "1px solid rgba(239, 68, 68, 0.3)", backgroundColor: "rgba(239, 68, 68, 0.03)" }}
            >
                <div className="roster-title flex items-center gap-2 text-rose-400">
                    <ShieldAlert size={18} style={{ color: "#ef4444" }} /> AUDITABLE MASTER CLEARANCE (DANGER ZONE)
                </div>
                <div className="query-help" style={{ marginBottom: "1rem" }}>
                    Purging audit logs removes historical compliance entries older than the selected threshold. Every purge action
                    automatically logs an immutable <strong>audit.purged</strong> event capturing who executed the clearance and how many
                    rows were purged.
                </div>
                <div className="query-controls-row">
                    <span className="admin-cell-muted">PURGE ENTRIES OLDER THAN:</span>
                    <input
                        type="number"
                        className="input-desktop w-28"
                        value={purgeDays}
                        onChange={(e) => setPurgeDays(Number(e.target.value))}
                        min={1}
                    />
                    <span className="admin-cell-muted">DAYS</span>
                    <button
                        className="btn-desktop"
                        onClick={handleVerifyAudit}
                        disabled={verifying}
                        title="Re-verify the tamper-evident hash chain over all audit rows"
                    >
                        {verifying ? <Loader2 className="animate-spin" size={18} /> : <ShieldCheck size={18} />}
                        <span>&nbsp;VERIFY CHAIN</span>
                    </button>
                    <button
                        className="btn-desktop-primary"
                        style={{ backgroundColor: "#dc2626", borderColor: "#b91c1c" }}
                        onClick={handlePurgeAudit}
                        disabled={purging}
                    >
                        {purging ? <Loader2 className="animate-spin" size={18} /> : <Trash2 size={18} />}
                        <span>&nbsp;EXECUTE AUDIT CLEARANCE</span>
                    </button>
                </div>
                {verifyResult && (
                    <div
                        className="query-help"
                        style={{
                            marginTop: "1rem",
                            border: `1px solid ${verifyResult.is_valid ? "rgba(34, 197, 94, 0.4)" : "rgba(239, 68, 68, 0.5)"}`,
                            color: verifyResult.is_valid ? "#4ade80" : "#f87171",
                        }}
                    >
                        {verifyResult.is_valid ? "✔ " : "✘ "}
                        {verifyResult.is_valid
                            ? `Chain VERIFIED — ${verifyResult.total_rows} rows, ${verifyResult.verified_checkpoints} checkpoints intact.`
                            : `Chain TAMPERED — first break at seq ${verifyResult.first_broken_seq ?? "?"} (row ${
                                  verifyResult.first_broken_id ?? "?"
                              }). ${verifyResult.message}`}
                    </div>
                )}
            </div>
        </div>
    );
}
