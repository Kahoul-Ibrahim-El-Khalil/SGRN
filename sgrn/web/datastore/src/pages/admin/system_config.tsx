import { useState, useEffect, useCallback, useMemo } from "react";
import { Loader2, Save, RefreshCw, TriangleAlert, Activity, Webhook, Plus, Trash2 } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import { authenticatedFetch } from "@/backend/api/fetcher";
import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import {
    fetchSystemConfig,
    updateSystemConfig,
    getFieldValue,
    setFieldValue,
    type SystemConfigDocument,
    type SystemConfigFieldSpec,
    type SystemConfigSaveResult,
} from "@/backend/api/system_config";

function LiveSessionsSection() {
    const [sessions, setSessions] = useState<any[]>([]);
    const [loading, setLoading] = useState(false);

    const loadSessions = useCallback(async () => {
        setLoading(true);
        try {
            const res = await authenticatedFetch(AdminBackendApiEndpoints.METAPROBE_SESSIONS);
            if (res.ok) {
                const data = await res.json();
                setSessions(Array.isArray(data) ? data : []);
            }
        } catch (e) {
            console.error(e);
        } finally {
            setLoading(false);
        }
    }, []);

    useEffect(() => {
        loadSessions();
        const timer = setInterval(loadSessions, 5000);
        return () => clearInterval(timer);
    }, [loadSessions]);

    return (
        <div className="admin-config-section mb-6">
            <div className="admin-config-section-header flex justify-between items-center">
                <div className="flex items-center gap-2">
                    <Activity className="text-emerald-500" size={20} />
                    <h3 className="font-semibold text-lg">Live Active Sessions ({sessions.length})</h3>
                </div>
                <button onClick={loadSessions} className="btn-desktop text-xs flex items-center gap-1">
                    <RefreshCw className={loading ? "animate-spin" : ""} size={14} /> Refresh
                </button>
            </div>
            <div className="admin-table-wrapper mt-3">
                <table className="admin-table text-sm w-full">
                    <thead>
                        <tr>
                            <th>Actor</th>
                            <th>Type</th>
                            <th>IP Address</th>
                            <th>Created At</th>
                            <th>Expires At</th>
                        </tr>
                    </thead>
                    <tbody>
                        {sessions.length === 0 ? (
                            <tr>
                                <td colSpan={5} className="text-center py-4 text-gray-500">
                                    No active sessions found
                                </td>
                            </tr>
                        ) : (
                            sessions.map((s, idx) => (
                                <tr key={idx}>
                                    <td className="font-mono">{s.actor_name}</td>
                                    <td>
                                        <span
                                            className={`badge-status ${s.actor_type === "automated_service" ? "badge-status--active" : ""}`}
                                        >
                                            {s.actor_type}
                                        </span>
                                    </td>
                                    <td className="font-mono">{s.ip}</td>
                                    <td className="text-xs text-gray-400">{s.created_at}</td>
                                    <td className="text-xs text-gray-400">{s.expires_at}</td>
                                </tr>
                            ))
                        )}
                    </tbody>
                </table>
            </div>
        </div>
    );
}

function WebhooksSection() {
    const [webhooks, setWebhooks] = useState<any[]>([]);
    const [url, setUrl] = useState("");
    const [secret, setSecret] = useState("");
    const [loading, setLoading] = useState(false);
    const [creating, setCreating] = useState(false);

    const loadWebhooks = useCallback(async () => {
        setLoading(true);
        try {
            const res = await authenticatedFetch(AdminBackendApiEndpoints.WEBHOOKS);
            if (res.ok) {
                const data = await res.json();
                setWebhooks(Array.isArray(data) ? data : []);
            }
        } catch (e) {
            console.error(e);
        } finally {
            setLoading(false);
        }
    }, []);

    useEffect(() => {
        loadWebhooks();
    }, [loadWebhooks]);

    const handleCreate = async (e: React.FormEvent) => {
        e.preventDefault();
        if (!url) return;
        setCreating(true);
        try {
            const res = await authenticatedFetch(AdminBackendApiEndpoints.WEBHOOKS, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ url, secret }),
            });
            if (res.ok) {
                setUrl("");
                setSecret("");
                await loadWebhooks();
            }
        } catch (e) {
            console.error(e);
        } finally {
            setCreating(false);
        }
    };

    const handleDelete = async (id: number) => {
        try {
            const res = await authenticatedFetch(AdminBackendApiEndpoints.WEBHOOK_ITEM(id), { method: "DELETE" });
            if (res.ok) {
                await loadWebhooks();
            }
        } catch (e) {
            console.error(e);
        }
    };

    return (
        <div className="admin-config-section mb-6">
            <div className="admin-config-section-header flex justify-between items-center">
                <div className="flex items-center gap-2">
                    <Webhook className="text-indigo-500" size={20} />
                    <h3 className="font-semibold text-lg">System Webhook Subscriptions</h3>
                </div>
                <button onClick={loadWebhooks} className="btn-desktop text-xs flex items-center gap-1">
                    <RefreshCw className={loading ? "animate-spin" : ""} size={14} /> Refresh
                </button>
            </div>

            <form onSubmit={handleCreate} className="flex gap-2 mt-3 mb-4">
                <input
                    type="url"
                    required
                    placeholder="https://example.com/webhook"
                    value={url}
                    onChange={(e) => setUrl(e.target.value)}
                    className="input-desktop flex-1 text-sm"
                />
                <input
                    type="text"
                    placeholder="Secret Key (Optional)"
                    value={secret}
                    onChange={(e) => setSecret(e.target.value)}
                    className="input-desktop w-48 text-sm"
                />
                <button type="submit" disabled={creating} className="btn-desktop-primary flex items-center gap-1 text-sm">
                    {creating ? <Loader2 className="animate-spin" size={16} /> : <Plus size={16} />} Register Webhook
                </button>
            </form>

            <div className="admin-table-wrapper">
                <table className="admin-table text-sm w-full">
                    <thead>
                        <tr>
                            <th>URL</th>
                            <th>Status</th>
                            <th>Created At</th>
                            <th className="text-right">Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        {webhooks.length === 0 ? (
                            <tr>
                                <td colSpan={4} className="text-center py-4 text-gray-500">
                                    No webhooks registered
                                </td>
                            </tr>
                        ) : (
                            webhooks.map((w) => (
                                <tr key={w.id}>
                                    <td className="font-mono text-xs">{w.url}</td>
                                    <td>
                                        <span className={`badge-status ${w.is_active ? "badge-status--active" : ""}`}>
                                            {w.is_active ? "ACTIVE" : "INACTIVE"}
                                        </span>
                                    </td>
                                    <td className="text-xs text-gray-400">{w.created_at}</td>
                                    <td className="text-right">
                                        <button onClick={() => handleDelete(w.id)} className="text-red-400 hover:text-red-600 p-1">
                                            <Trash2 size={16} />
                                        </button>
                                    </td>
                                </tr>
                            ))
                        )}
                    </tbody>
                </table>
            </div>
        </div>
    );
}

function isDirty(t_original: any, t_current: any): boolean {
    return JSON.stringify(t_original ?? null) !== JSON.stringify(t_current ?? null);
}

function FieldEditor({
    t_spec,
    t_value,
    t_on_change,
}: {
    t_spec: SystemConfigFieldSpec;
    t_value: any;
    t_on_change: (t_next: any) => void;
}) {
    if (t_spec.type === "bool") {
        return (
            <label className="admin-config-check">
                <input type="checkbox" checked={t_value === true} onChange={(e) => t_on_change(e.target.checked)} />
                <span>{t_value === true ? "ON" : "OFF"}</span>
            </label>
        );
    }
    if (t_spec.type === "string[]") {
        const text = Array.isArray(t_value) ? t_value.join(", ") : "";
        return (
            <input
                className="input-desktop flex-1"
                value={text}
                placeholder={t_value !== undefined ? "" : "—"}
                onChange={(e) => {
                    const items = e.target.value
                        .split(",")
                        .map((s) => s.trim())
                        .filter((s) => s.length > 0);
                    t_on_change(items);
                }}
            />
        );
    }
    if (t_spec.type === "uint" || t_spec.type === "double") {
        return (
            <input
                className="input-desktop w-64"
                type="number"
                step={t_spec.type === "double" ? "0.1" : "1"}
                min={t_spec.min}
                max={t_spec.max}
                value={t_value ?? ""}
                placeholder={t_value !== undefined ? "" : "—"}
                onChange={(e) => {
                    if (e.target.value === "") {
                        t_on_change(undefined);
                        return;
                    }
                    t_on_change(t_spec.type === "double" ? Number(e.target.value) : Math.trunc(Number(e.target.value)));
                }}
            />
        );
    }
    return (
        <input
            className="input-desktop flex-1"
            type="text"
            value={t_value ?? ""}
            placeholder={t_value !== undefined ? "" : "—"}
            onChange={(e) => t_on_change(e.target.value)}
        />
    );
}

export default function SystemConfigPanel() {
    const { showEvent } = useEvent();
    const [doc, setDoc] = useState<SystemConfigDocument | null>(null);
    const [draft, setDraft] = useState<Record<string, any>>({});
    const [loading, setLoading] = useState(false);
    const [saving, setSaving] = useState(false);
    const [lastResult, setLastResult] = useState<SystemConfigSaveResult | null>(null);

    const load = useCallback(async () => {
        setLoading(true);
        setLastResult(null);
        try {
            const result = await fetchSystemConfig();
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load system config");
            }
            setDoc(result.data);
            setDraft(JSON.parse(JSON.stringify(result.data.values ?? {})));
        } catch (error) {
            console.error("Failed to fetch system config:", error);
            showEvent("error", "Unable to load system config");
        } finally {
            setLoading(false);
        }
    }, [showEvent]);

    useEffect(() => {
        load();
    }, [load]);

    const sections = useMemo(() => {
        const order: string[] = [];
        const grouped = new Map<string, SystemConfigFieldSpec[]>();
        for (const spec of doc?.schema ?? []) {
            if (!grouped.has(spec.section)) {
                grouped.set(spec.section, []);
                order.push(spec.section);
            }
            grouped.get(spec.section)!.push(spec);
        }
        return order.map((name) => ({ name, fields: grouped.get(name)! }));
    }, [doc]);

    const dirtyPaths = useMemo(() => {
        if (!doc) return [];
        return doc.schema.filter((s) => isDirty(getFieldValue(doc.values, s.path), getFieldValue(draft, s.path))).map((s) => s.path);
    }, [doc, draft]);

    const handleSave = useCallback(async () => {
        if (!doc || dirtyPaths.length === 0) return;
        setSaving(true);
        try {
            // Send only changed leaves: smaller blast radius, clearer report.
            let diff: Record<string, any> = {};
            for (const path of dirtyPaths) {
                diff = setFieldValue(diff, path, getFieldValue(draft, path));
            }
            const result = await updateSystemConfig(diff);
            if (result.error || !result.data) {
                throw new Error(result.error || "Save failed");
            }
            setLastResult(result.data);
            if (result.data.applied_hot.length > 0) {
                showEvent("success", `${result.data.applied_hot.length} setting(s) applied live`);
            }
            if (result.data.restart_required.length > 0) {
                showEvent("warning", `${result.data.restart_required.length} setting(s) need a restart`);
            }
            if (result.data.warnings.length > 0) {
                for (const w of result.data.warnings) showEvent("warning", w);
            }
            await load();
        } catch (error) {
            console.error("System config save failed:", error);
            showEvent("error", error instanceof Error ? error.message : "System config save failed");
        } finally {
            setSaving(false);
        }
    }, [doc, draft, dirtyPaths, load, showEvent]);

    if (loading && !doc) {
        return (
            <div className="query-builder">
                <Loader2 className="animate-spin" size={18} /> <span>&nbsp;LOADING SYSTEM CONFIG…</span>
            </div>
        );
    }

    if (!doc) {
        return (
            <div className="query-builder">
                <div className="empty-state">System config unavailable.</div>
                <button className="btn-desktop" onClick={load}>
                    <RefreshCw size={14} /> <span>&nbsp;RETRY</span>
                </button>
            </div>
        );
    }

    return (
        <div className="query-builder">
            <div className="query-controls">
                <div className="query-controls-row">
                    <span className="admin-cell-muted">CONFIG FILE</span>
                    <code className="admin-cell-primary">{doc.config_path}</code>
                    <span className="admin-cell-muted">BACKUP ON SAVE → {doc.config_path}.bak</span>
                    <button className="btn-desktop" onClick={load} disabled={loading}>
                        <RefreshCw size={14} className={loading ? "drive-spin" : ""} />
                    </button>
                </div>
                <div className="query-controls-row">
                    <span className="admin-cell-muted">EFFECTIVE NOW</span>
                    <span className="admin-cell-bold">
                        multipart ≥ {getFieldValue(doc.live, "custom_config.s3.chunking_threshold_mb") ?? "?"} MB (original size)
                    </span>
                    <span className="admin-cell-muted">
                        max {getFieldValue(doc.live, "custom_config.s3.max_file_size_mb") ?? "?"} MB · part{" "}
                        {getFieldValue(doc.live, "custom_config.s3.chunk_part_size_mb") ?? "?"} MB
                    </span>
                </div>
            </div>

            {doc.pending_restart.length > 0 && (
                <div className="roster-card">
                    <div className="roster-title">
                        <TriangleAlert size={14} /> PENDING RESTART — {doc.pending_restart.length} SETTING(S) SAVED BUT NOT LIVE
                    </div>
                    <div className="explorer-body">
                        <pre>{doc.pending_restart.join("\n")}</pre>
                    </div>
                </div>
            )}

            {sections.map((section) => (
                <div className="roster-card" key={section.name}>
                    <div className="roster-title">{section.name.toUpperCase()}</div>
                    <div className="datagrid-wrapper">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>SETTING</th>
                                    <th>VALUE</th>
                                    <th>APPLY</th>
                                </tr>
                            </thead>
                            <tbody>
                                {section.fields.map((spec) => {
                                    const original = getFieldValue(doc.values, spec.path);
                                    const current = getFieldValue(draft, spec.path);
                                    const dirty = isDirty(original, current);
                                    return (
                                        <tr key={spec.path} className={dirty ? "admin-row-dirty" : ""}>
                                            <td>
                                                <div className="admin-cell-bold">{spec.label}</div>
                                                <div className="admin-cell-muted">{spec.path}</div>
                                            </td>
                                            <td>
                                                <FieldEditor
                                                    t_spec={spec}
                                                    t_value={current}
                                                    t_on_change={(next) => setDraft((d) => setFieldValue(d, spec.path, next))}
                                                />
                                            </td>
                                            <td>
                                                {spec.hot ? (
                                                    <span className="badge-status badge-status--active">LIVE</span>
                                                ) : (
                                                    <span className="badge-status">RESTART</span>
                                                )}
                                            </td>
                                        </tr>
                                    );
                                })}
                            </tbody>
                        </table>
                    </div>
                </div>
            ))}

            <div className="query-controls">
                <div className="query-controls-row">
                    <button className="btn-desktop-primary" onClick={handleSave} disabled={saving || dirtyPaths.length === 0}>
                        {saving ? <Loader2 className="animate-spin" size={18} /> : <Save size={18} />}
                        <span>&nbsp;SAVE {dirtyPaths.length > 0 ? `(${dirtyPaths.length} CHANGED)` : ""}</span>
                    </button>
                    {dirtyPaths.length > 0 && (
                        <button
                            className="btn-desktop"
                            onClick={() => {
                                if (doc) setDraft(JSON.parse(JSON.stringify(doc.values ?? {})));
                            }}
                        >
                            DISCARD
                        </button>
                    )}
                </div>
            </div>

            {lastResult && (
                <div className="json-explorer">
                    <div className="explorer-header">
                        <div className="explorer-header-left">
                            <span className="explorer-label">LAST SAVE RESULT</span>
                        </div>
                    </div>
                    <div className="explorer-body">
                        <pre>{JSON.stringify(lastResult, null, 2)}</pre>
                    </div>
                </div>
            )}

            <LiveSessionsSection />
            <WebhooksSection />

            <div className="query-help">
                <strong>LIVE</strong> settings apply on save without a restart. <strong>RESTART</strong> settings are written to{" "}
                {doc.config_path} (previous file kept as .bak, also preserved across --init) and take effect on restart. Secrets (passwords,
                keys, tokens) are never shown or editable here — edit the file directly for those.
            </div>
        </div>
    );
}
