import { useState, useCallback, useEffect } from "react";
import { Loader2, Search, Plus, Trash2, Save, Lock, ShieldCheck, RefreshCw } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import { QueryListBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch } from "@/backend/api/fetcher";
import {
    fetchFormats,
    upsertFormat,
    deleteFormat,
    fetchQuotas,
    updateQuota,
    bytesToMb,
    mbToBytes,
    type StorageFormat,
    type QuotaKind,
    type QuotaRow,
} from "@/backend/api/governance";

export function FormatsPanel() {
    const { showEvent } = useEvent();
    const [formats, setFormats] = useState<StorageFormat[]>([]);
    const [search, setSearch] = useState("");
    const [allowedFilter, setAllowedFilter] = useState("");
    const [loading, setLoading] = useState(false);
    const [saving, setSaving] = useState(false);
    const [armingDelete, setArmingDelete] = useState<string | null>(null);

    const [newExt, setNewExt] = useState("");
    const [newMime, setNewMime] = useState("");
    const [newCompressed, setNewCompressed] = useState(true);
    const [newAllowed, setNewAllowed] = useState(true);
    const [newDesc, setNewDesc] = useState("");

    const load = useCallback(async () => {
        setLoading(true);
        try {
            const result = await fetchFormats(search, allowedFilter);
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load formats");
            }
            setFormats(result.data.formats);
        } catch (error) {
            console.error("Failed to fetch formats:", error);
            showEvent("error", "Unable to load formats registry");
        } finally {
            setLoading(false);
        }
    }, [search, allowedFilter, showEvent]);

    const handleAdd = useCallback(async () => {
        if (!newExt.trim() || !newMime.trim()) {
            showEvent("error", "Extension and MIME type are required");
            return;
        }
        setSaving(true);
        try {
            const result = await upsertFormat({
                extension: newExt.trim().toLowerCase(),
                mime_type: newMime.trim(),
                is_compressed: newCompressed,
                is_allowed: newAllowed,
                description: newDesc.trim() || undefined,
            });
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to save format");
            }
            showEvent("success", result.data.created ? `Format ${result.data.extension} added` : `Format ${result.data.extension} updated`);
            setNewExt("");
            setNewMime("");
            setNewDesc("");
            await load();
        } catch (error) {
            console.error("Format save failed:", error);
            showEvent("error", error instanceof Error ? error.message : "Format save failed");
        } finally {
            setSaving(false);
        }
    }, [newExt, newMime, newCompressed, newAllowed, newDesc, load, showEvent]);

    const handleToggleAllowed = useCallback(
        async (t_format: StorageFormat) => {
            try {
                const result = await upsertFormat({
                    extension: t_format.extension,
                    mime_type: t_format.mime_type,
                    is_compressed: t_format.is_compressed,
                    is_allowed: !t_format.is_allowed,
                });
                if (result.error) {
                    throw new Error(result.error);
                }
                showEvent("success", `${t_format.extension} ${!t_format.is_allowed ? "allowed" : "blocked"}`);
                await load();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : "Toggle failed");
            }
        },
        [load, showEvent],
    );

    const handleDelete = useCallback(
        async (t_ext: string) => {
            if (armingDelete !== t_ext) {
                setArmingDelete(t_ext);
                return;
            }
            setArmingDelete(null);
            try {
                const result = await deleteFormat(t_ext);
                if (result.error) {
                    throw new Error(result.error);
                }
                showEvent("success", `Format ${t_ext} removed`);
                await load();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : "Delete failed");
            }
        },
        [armingDelete, load, showEvent],
    );

    return (
        <div className="query-builder">
            <div className="query-controls">
                <div className="query-controls-row">
                    <input
                        className="input-desktop flex-1"
                        placeholder="SEARCH EXTENSION OR MIME"
                        value={search}
                        onChange={(e) => setSearch(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === "Enter") load();
                        }}
                    />
                    <select className="input-desktop w-64" value={allowedFilter} onChange={(e) => setAllowedFilter(e.target.value)}>
                        <option value="">ALLOWED: ANY</option>
                        <option value="true">ALLOWED ONLY</option>
                        <option value="false">BLOCKED ONLY</option>
                    </select>
                    <button className="btn-desktop-primary w-12" onClick={load} disabled={loading}>
                        {loading ? <Loader2 className="animate-spin" size={18} /> : <Search size={18} />}
                    </button>
                </div>
            </div>

            {formats.length > 0 && (
                <div className="roster-card">
                    <div className="roster-title">
                        FORMATS REGISTRY — {formats.length} ENTR{formats.length === 1 ? "Y" : "IES"}
                    </div>
                    <div className="datagrid-wrapper">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>EXTENSION</th>
                                    <th>MIME TYPE</th>
                                    <th>COMPRESSED</th>
                                    <th>UPLOADS</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {formats.map((f) => (
                                    <tr key={f.extension}>
                                        <td className="admin-cell-primary">{f.extension}</td>
                                        <td>{f.mime_type}</td>
                                        <td>{f.is_compressed ? "YES" : "NO"}</td>
                                        <td>
                                            {f.is_allowed ? (
                                                <span className="badge-status badge-status--active">ALLOWED</span>
                                            ) : (
                                                <span className="badge-status">BLOCKED</span>
                                            )}
                                        </td>
                                        <td>
                                            <div className="admin-actions-right">
                                                <button className="btn-desktop h-7 px-3" onClick={() => handleToggleAllowed(f)}>
                                                    {f.is_allowed ? "BLOCK" : "ALLOW"}
                                                </button>
                                                <button
                                                    className={`btn-desktop h-7 px-3 ${armingDelete === f.extension ? "btn-desktop-primary" : ""}`}
                                                    onClick={() => handleDelete(f.extension)}
                                                >
                                                    {armingDelete === f.extension ? "CONFIRM" : <Trash2 size={14} />}
                                                </button>
                                            </div>
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                </div>
            )}

            <div className="roster-card">
                <div className="roster-title">ADD / CURATE FORMAT</div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop w-64"
                        placeholder="EXTENSION (e.g. parquet)"
                        value={newExt}
                        onChange={(e) => setNewExt(e.target.value)}
                        maxLength={15}
                    />
                    <input
                        className="input-desktop flex-1"
                        placeholder="MIME TYPE (e.g. application/vnd.apache.parquet)"
                        value={newMime}
                        onChange={(e) => setNewMime(e.target.value)}
                        maxLength={128}
                    />
                    <label className="admin-config-check">
                        <input type="checkbox" checked={newCompressed} onChange={(e) => setNewCompressed(e.target.checked)} />
                        <span>COMPRESSED</span>
                    </label>
                    <label className="admin-config-check">
                        <input type="checkbox" checked={newAllowed} onChange={(e) => setNewAllowed(e.target.checked)} />
                        <span>ALLOWED</span>
                    </label>
                    <button className="btn-desktop-primary" onClick={handleAdd} disabled={saving}>
                        {saving ? <Loader2 className="animate-spin" size={18} /> : <Plus size={18} />}
                        <span>&nbsp;SAVE</span>
                    </button>
                </div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop flex-1"
                        placeholder="DESCRIPTION (optional)"
                        value={newDesc}
                        onChange={(e) => setNewDesc(e.target.value)}
                    />
                </div>
            </div>

            <div className="query-help">
                <strong>Blocked</strong> formats reject new uploads at the service layer; history stays readable. <strong>Delete</strong> is
                refused while files still reference the extension — block first, purge history through garbage collection, then delete.
                Files whose extension is not listed here are stored with a null format — the suffix stays part of the file name. Add an
                entry to recognize it.
            </div>
        </div>
    );
}

interface QuotaDraft {
    storageMb: string;
    storageUnlimited: boolean;
    entries: string;
    entriesUnlimited: boolean;
    maxFileMb: string;
    chunkMb: string;
    uploadRpm: string;
    rateRpm: string;
}

function draftKey(t_row: QuotaRow, t_kind: QuotaKind): string {
    if (t_kind === "organisation") return `org:${t_row.name}`;
    return `${t_kind}:${t_row.id}`;
}

export function QuotasPanel() {
    const { showEvent } = useEvent();
    const [kind, setKind] = useState<QuotaKind>("user");
    const [search, setSearch] = useState("");
    const [rows, setRows] = useState<QuotaRow[]>([]);
    const [loading, setLoading] = useState(false);
    const [drafts, setDrafts] = useState<Record<string, QuotaDraft>>({});
    const [savingKey, setSavingKey] = useState<string | null>(null);

    const load = useCallback(async () => {
        setLoading(true);
        setDrafts({});
        try {
            const result = await fetchQuotas(kind, search);
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load quotas");
            }
            setRows(result.data.rows);
        } catch (error) {
            console.error("Failed to fetch quotas:", error);
            showEvent("error", "Unable to load quota ledger");
        } finally {
            setLoading(false);
        }
    }, [kind, search, showEvent]);

    const draftFor = (t_row: QuotaRow): QuotaDraft => {
        const key = draftKey(t_row, kind);
        return (
            drafts[key] ?? {
                storageMb: bytesToMb(t_row.storage_limit_bytes),
                storageUnlimited: t_row.storage_limit_bytes === null,
                entries: t_row.entry_count_limit === null ? "" : String(t_row.entry_count_limit),
                entriesUnlimited: t_row.entry_count_limit === null,
                maxFileMb: t_row.max_file_size_mb === null || t_row.max_file_size_mb === undefined ? "" : String(t_row.max_file_size_mb),
                chunkMb:
                    t_row.preferred_chunk_size_mb === null || t_row.preferred_chunk_size_mb === undefined
                        ? ""
                        : String(t_row.preferred_chunk_size_mb),
                uploadRpm:
                    t_row.rate_limit_upload_rpm === null || t_row.rate_limit_upload_rpm === undefined
                        ? ""
                        : String(t_row.rate_limit_upload_rpm),
                rateRpm: t_row.rate_limit_rpm === null || t_row.rate_limit_rpm === undefined ? "" : String(t_row.rate_limit_rpm),
            }
        );
    };

    const setDraft = (t_row: QuotaRow, t_patch: Partial<QuotaDraft>) => {
        const key = draftKey(t_row, kind);
        setDrafts((d) => ({ ...d, [key]: { ...draftFor(t_row), ...t_patch } }));
    };

    const handleSave = useCallback(
        async (t_row: QuotaRow) => {
            const key = draftKey(t_row, kind);
            const draft: QuotaDraft = drafts[key] ?? draftFor(t_row);
            const storageMb = mbToBytes(draft.storageMb);
            if (!draft.storageUnlimited && storageMb === null) {
                showEvent("error", "Storage cap must be a non-negative MB value or unlimited");
                return;
            }
            let entriesVal: number | null = null;
            if (!draft.entriesUnlimited) {
                if (draft.entries.trim() === "" || !Number.isInteger(Number(draft.entries)) || Number(draft.entries) < 0) {
                    showEvent("error", "Entry cap must be a non-negative integer or unlimited");
                    return;
                }
                entriesVal = Number(draft.entries);
            }

            const parseNullableNum = (v: string): number | null => {
                const trimmed = v.trim();
                if (!trimmed) return null;
                const n = Number(trimmed);
                return Number.isFinite(n) && n >= 0 ? Math.floor(n) : null;
            };

            setSavingKey(key);
            try {
                const body: {
                    kind: QuotaKind;
                    id?: number;
                    name?: string;
                    storage_limit_bytes?: number | null;
                    entry_count_limit?: number | null;
                    max_file_size_mb?: number | null;
                    preferred_chunk_size_mb?: number | null;
                    rate_limit_upload_rpm?: number | null;
                    rate_limit_rpm?: number | null;
                } = {
                    kind,
                    storage_limit_bytes: draft.storageUnlimited ? null : storageMb,
                    entry_count_limit: draft.entriesUnlimited ? null : entriesVal,
                    max_file_size_mb: parseNullableNum(draft.maxFileMb),
                    preferred_chunk_size_mb: parseNullableNum(draft.chunkMb),
                    rate_limit_upload_rpm: parseNullableNum(draft.uploadRpm),
                    rate_limit_rpm: parseNullableNum(draft.rateRpm),
                };
                if (kind === "organisation") {
                    body.name = t_row.name;
                } else {
                    body.id = t_row.id;
                }
                const result = await updateQuota(body);
                if (result.error || !result.data) {
                    throw new Error(result.error || "Quota update failed");
                }
                showEvent("success", `Quota updated for ${result.data.label ?? key}`);
                setDrafts((d) => {
                    const next = { ...d };
                    delete next[key];
                    return next;
                });
                await load();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : "Quota update failed");
            } finally {
                setSavingKey(null);
            }
        },
        [kind, drafts, load, showEvent],
    );

    const labelOf = (t_row: QuotaRow) => t_row.email ?? t_row.name ?? `#${t_row.id}`;

    return (
        <div className="query-builder">
            <div className="query-controls">
                <div className="query-controls-row">
                    <select className="input-desktop w-64" value={kind} onChange={(e) => setKind(e.target.value as QuotaKind)}>
                        <option value="user">USERS</option>
                        <option value="service">SERVICES</option>
                        <option value="organisation">ORGANISATIONS</option>
                    </select>
                    <input
                        className="input-desktop flex-1"
                        placeholder={kind === "organisation" ? "SEARCH ORGANISATION" : "SEARCH EMAIL / NAME / ORGANISATION"}
                        value={search}
                        onChange={(e) => setSearch(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === "Enter") load();
                        }}
                    />
                    <button className="btn-desktop-primary w-12" onClick={load} disabled={loading}>
                        {loading ? <Loader2 className="animate-spin" size={18} /> : <Search size={18} />}
                    </button>
                </div>
            </div>

            {rows.length > 0 && (
                <div className="roster-card">
                    <div className="roster-title">
                        QUOTA & CONSTRAINT LEDGER — {kind.toUpperCase()} — {rows.length} ROW{rows.length === 1 ? "" : "S"}
                    </div>
                    <div className="datagrid-wrapper">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>{kind === "organisation" ? "ORGANISATION" : kind === "user" ? "EMAIL" : "SERVICE"}</th>
                                    <th>USED / STORAGE CAP (MB)</th>
                                    <th>ENTRIES / ENTRY CAP</th>
                                    <th>MAX FILE (MB)</th>
                                    <th>CHUNK (MB)</th>
                                    <th>UPLOAD RPM</th>
                                    <th>API RPM</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {rows.map((r) => {
                                    const key = draftKey(r, kind);
                                    const draft = draftFor(r);
                                    return (
                                        <tr key={key}>
                                            <td>
                                                <div className="admin-cell-bold">{labelOf(r)}</div>
                                                {r.organisation && <div className="admin-cell-muted">{r.organisation}</div>}
                                            </td>
                                            <td>
                                                <div className="query-controls-row">
                                                    <span className="text-xs text-muted-foreground mr-1">
                                                        {bytesToMb(r.storage_used_bytes)} /
                                                    </span>
                                                    <input
                                                        className="input-desktop w-28"
                                                        type="number"
                                                        min={0}
                                                        step="any"
                                                        placeholder="∞"
                                                        disabled={draft.storageUnlimited}
                                                        value={draft.storageUnlimited ? "" : draft.storageMb}
                                                        onChange={(e) => setDraft(r, { storageMb: e.target.value })}
                                                    />
                                                    <label className="admin-config-check">
                                                        <input
                                                            type="checkbox"
                                                            checked={draft.storageUnlimited}
                                                            onChange={(e) => setDraft(r, { storageUnlimited: e.target.checked })}
                                                        />
                                                        <span>∞</span>
                                                    </label>
                                                </div>
                                            </td>
                                            <td>
                                                <div className="query-controls-row">
                                                    <span className="text-xs text-muted-foreground mr-1">{r.entries_used} /</span>
                                                    <input
                                                        className="input-desktop w-24"
                                                        type="number"
                                                        min={0}
                                                        step={1}
                                                        placeholder="∞"
                                                        disabled={draft.entriesUnlimited}
                                                        value={draft.entriesUnlimited ? "" : draft.entries}
                                                        onChange={(e) => setDraft(r, { entries: e.target.value })}
                                                    />
                                                    <label className="admin-config-check">
                                                        <input
                                                            type="checkbox"
                                                            checked={draft.entriesUnlimited}
                                                            onChange={(e) => setDraft(r, { entriesUnlimited: e.target.checked })}
                                                        />
                                                        <span>∞</span>
                                                    </label>
                                                </div>
                                            </td>
                                            <td>
                                                <input
                                                    className="input-desktop w-24"
                                                    type="number"
                                                    min={1}
                                                    placeholder="SYS"
                                                    value={draft.maxFileMb}
                                                    onChange={(e) => setDraft(r, { maxFileMb: e.target.value })}
                                                />
                                            </td>
                                            <td>
                                                <input
                                                    className="input-desktop w-20"
                                                    type="number"
                                                    min={1}
                                                    placeholder="5"
                                                    value={draft.chunkMb}
                                                    onChange={(e) => setDraft(r, { chunkMb: e.target.value })}
                                                />
                                            </td>
                                            <td>
                                                <input
                                                    className="input-desktop w-24"
                                                    type="number"
                                                    min={1}
                                                    placeholder="10000"
                                                    value={draft.uploadRpm}
                                                    onChange={(e) => setDraft(r, { uploadRpm: e.target.value })}
                                                />
                                            </td>
                                            <td>
                                                <input
                                                    className="input-desktop w-24"
                                                    type="number"
                                                    min={1}
                                                    placeholder="SYS"
                                                    value={draft.rateRpm}
                                                    onChange={(e) => setDraft(r, { rateRpm: e.target.value })}
                                                />
                                            </td>
                                            <td>
                                                <div className="admin-actions-right">
                                                    <button
                                                        className="btn-desktop h-7 px-3"
                                                        onClick={() => handleSave(r)}
                                                        disabled={savingKey === key}
                                                    >
                                                        {savingKey === key ? (
                                                            <Loader2 className="animate-spin" size={14} />
                                                        ) : (
                                                            <Save size={14} />
                                                        )}
                                                    </button>
                                                </div>
                                            </td>
                                        </tr>
                                    );
                                })}
                            </tbody>
                        </table>
                    </div>
                </div>
            )}

            <div className="query-help">
                <strong>Granular Constraints</strong> allow setting custom Storage Quotas, Maximum File Size (MB), Upload Chunk Size (MB),
                Upload Rate (RPM), and General API Rate (RPM) per User, Service, or Organisation. Empty fields seamlessly fall back to
                system defaults.
            </div>
        </div>
    );
}

interface DomainRecord {
    id: number;
    name: string;
    path: string;
    parent_id?: number | null;
    encryption_policy?: "none" | "optional" | "required" | string;
}

export function EncryptionPoliciesPanel() {
    const { showEvent } = useEvent();
    const [domains, setDomains] = useState<DomainRecord[]>([]);
    const [policies, setPolicies] = useState<Record<number, string>>({});
    const [loading, setLoading] = useState(false);
    const [savingId, setSavingId] = useState<number | null>(null);

    const load = useCallback(async () => {
        setLoading(true);
        try {
            const res = await authenticatedFetch(QueryListBackendApiEndpoints.LIST_DOMAINS);
            if (!res.ok) {
                throw new Error(`Failed to load domains (HTTP ${res.status})`);
            }
            const data = await res.json();
            if (Array.isArray(data)) {
                setDomains(data);
                const map: Record<number, string> = {};
                for (const d of data) {
                    map[d.id] = d.encryption_policy || "optional";
                }
                setPolicies(map);
            }
        } catch (err: any) {
            console.error("Failed to load domain encryption policies:", err);
            showEvent("error", err?.message || "Unable to load domain encryption policies");
        } finally {
            setLoading(false);
        }
    }, [showEvent]);

    useEffect(() => {
        load();
    }, [load]);

    const handleSave = useCallback(
        async (domain: DomainRecord) => {
            const newPolicy = policies[domain.id] || "optional";
            setSavingId(domain.id);
            try {
                const res = await authenticatedFetch(`/api/v1/domains/${domain.id}`, {
                    method: "PATCH",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ encryption_policy: newPolicy }),
                });
                if (!res.ok) {
                    const err = await res.json().catch(() => ({}));
                    throw new Error(err.message || `Failed to update policy (HTTP ${res.status})`);
                }
                showEvent("success", `Domain "${domain.name}" encryption policy set to: ${newPolicy}`);
                await load();
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to update encryption policy");
            } finally {
                setSavingId(null);
            }
        },
        [policies, load, showEvent],
    );

    return (
        <div className="governance-panel">
            <div className="query-controls-row flex items-center justify-between">
                <div className="flex items-center gap-2">
                    <ShieldCheck size={18} className="text-primary" />
                    <span className="font-semibold text-sm tracking-wider">DOMAIN ENCRYPTION POLICIES</span>
                </div>
                <button className="btn-desktop h-8 px-3" onClick={load} disabled={loading} title="Reload domains">
                    <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
                    <span>&nbsp;RELOAD</span>
                </button>
            </div>

            {loading && domains.length === 0 ? (
                <div className="governance-loading">
                    <Loader2 className="animate-spin" size={24} />
                    <span>LOADING DOMAIN POLICIES...</span>
                </div>
            ) : domains.length === 0 ? (
                <div className="governance-empty">NO DOMAINS FOUND</div>
            ) : (
                <div className="roster-card">
                    <div className="roster-table-wrap">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>DOMAIN</th>
                                    <th>PATH</th>
                                    <th>ENCRYPTION POLICY</th>
                                    <th>STATUS</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {domains.map((d) => {
                                    const currentPolicy = policies[d.id] ?? (d.encryption_policy || "optional");
                                    const hasChanged = currentPolicy !== (d.encryption_policy || "optional");
                                    return (
                                        <tr key={d.id}>
                                            <td className="admin-cell-primary">{d.name}</td>
                                            <td className="font-mono text-xs text-muted-foreground">{d.path || "/"}</td>
                                            <td>
                                                <select
                                                    className="input-desktop text-xs py-1 px-2 h-7"
                                                    value={currentPolicy}
                                                    onChange={(e) =>
                                                        setPolicies((prev) => ({
                                                            ...prev,
                                                            [d.id]: e.target.value,
                                                        }))
                                                    }
                                                >
                                                    <option value="none">none (Plaintext only)</option>
                                                    <option value="optional">optional (Client selects)</option>
                                                    <option value="required">required (Enforce AES-256-GCM)</option>
                                                </select>
                                            </td>
                                            <td>
                                                {currentPolicy === "required" ? (
                                                    <span className="badge-status badge-status--active">
                                                        <Lock size={10} className="inline mr-1" />
                                                        ENFORCED
                                                    </span>
                                                ) : currentPolicy === "optional" ? (
                                                    <span className="badge-status">OPTIONAL</span>
                                                ) : (
                                                    <span className="badge-status text-muted-foreground">DISABLED</span>
                                                )}
                                            </td>
                                            <td>
                                                <div className="admin-actions-right">
                                                    <button
                                                        className={`btn-desktop h-7 px-3 ${hasChanged ? "btn-desktop-primary" : ""}`}
                                                        onClick={() => handleSave(d)}
                                                        disabled={savingId === d.id || !hasChanged}
                                                        title={hasChanged ? "Save policy changes" : "No changes to save"}
                                                    >
                                                        {savingId === d.id ? (
                                                            <Loader2 className="animate-spin" size={14} />
                                                        ) : (
                                                            <Save size={14} />
                                                        )}
                                                        <span>&nbsp;SAVE</span>
                                                    </button>
                                                </div>
                                            </td>
                                        </tr>
                                    );
                                })}
                            </tbody>
                        </table>
                    </div>
                </div>
            )}

            <div className="query-help">
                <strong>Encryption Policies:</strong>
                <ul className="list-disc pl-5 mt-1 space-y-1">
                    <li>
                        <strong>none:</strong> Encryption requests are ignored; files are stored in plaintext. Presigned S3 URLs are
                        supported.
                    </li>
                    <li>
                        <strong>optional (default):</strong> Uploads are encrypted when requested by the client (
                        <code className="font-mono">encrypt: true</code>) and plaintext otherwise.
                    </li>
                    <li>
                        <strong>required:</strong> Every file uploaded to this domain is strictly encrypted with AES-256-GCM AEAD at rest
                        with its own unique DEK stored in the hardware-protected key store. Deduplication is automatically bypassed to
                        ensure maximum data privacy.
                    </li>
                </ul>
            </div>
        </div>
    );
}
