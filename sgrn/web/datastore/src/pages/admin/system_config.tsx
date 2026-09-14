import { useState, useEffect, useCallback, useMemo } from "react";
import { Loader2, Save, RefreshCw, TriangleAlert } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import {
    fetchSystemConfig,
    updateSystemConfig,
    getFieldValue,
    setFieldValue,
    type SystemConfigDocument,
    type SystemConfigFieldSpec,
    type SystemConfigSaveResult,
} from "@/backend/api/system_config";

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

            <div className="query-help">
                <strong>LIVE</strong> settings apply on save without a restart. <strong>RESTART</strong> settings are written to{" "}
                {doc.config_path} (previous file kept as .bak, also preserved across --init) and take effect on restart. Secrets (passwords,
                keys, tokens) are never shown or editable here — edit the file directly for those.
            </div>
        </div>
    );
}
