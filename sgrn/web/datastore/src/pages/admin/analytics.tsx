import { useState, useCallback } from "react";
import { Loader2, Search, RefreshCw } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import {
    fetchAnalyticsOverview,
    fetchAnalyticsBreakdown,
    formatBytes,
    type AnalyticsOverview,
    type BreakdownKind,
    type BreakdownRow,
} from "@/backend/api/analytics";

function StatCell({ t_label, t_value, t_sub }: { t_label: string; t_value: string; t_sub?: string }) {
    return (
        <div className="roster-card">
            <div className="admin-cell-muted">{t_label}</div>
            <div className="admin-cell-bold">{t_value}</div>
            {t_sub && <div className="admin-cell-muted">{t_sub}</div>}
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
                    <div className="query-controls-row">
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
                            <div className="explorer-body">
                                {overview.timeseries_30d.map((p) => (
                                    <div key={p.day} className="query-controls-row">
                                        <span className="admin-cell-muted w-64">{p.day}</span>
                                        <div className="flex-1" style={{ background: "var(--panel, #eee)", height: 10 }}>
                                            <div
                                                style={{
                                                    width: `${Math.round((p.files / maxDayFiles) * 100)}%`,
                                                    height: 10,
                                                    background: "var(--primary, #2563eb)",
                                                }}
                                            />
                                        </div>
                                        <span className="admin-cell-bold w-64">
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
        </div>
    );
}
