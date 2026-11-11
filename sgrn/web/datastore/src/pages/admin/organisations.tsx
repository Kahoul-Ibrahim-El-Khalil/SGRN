// OrganisationsPanel — organisation lifecycle for the admin dashboard.
//
// Backend contract (admin-organisations.cpp):
//   GET   /api/v1/admin/organisations        list (platform: all; org admin: own row)
//   POST  /api/v1/admin/organisations        create + optional initial-admin bootstrap
//   PATCH /api/v1/admin/organisations/{name} update description / default domain / status
//
// Organisation names render into "<organisation>-<domain>" authorities, so
// '.' '@' '/' '-' and whitespace are rejected server-side — the form
// validates the same rule up front for instant feedback.
import { useCallback, useEffect, useState } from "react";
import { Building2, Loader2, Plus, RefreshCw, Save } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch } from "@/backend/api/fetcher";
import { bytesToMb, mbToBytes } from "@/backend/api/governance";

interface OrganisationRow {
    name: string;
    description: string | null;
    domain: string | null;
    status: string;
    total_virtual_size: number;
    storage_limit: number | null;
    total_entry_count: number;
    entry_count_limit: number | null;
    rate_limit_rpm: number | null;
}

interface OrgDraft {
    description: string;
    domain: string;
    status: string;
}

const STATUSES = ["active", "suspended", "archived"] as const;

const STATUS_BADGE: Record<string, string> = {
    active: "badge-status badge-status--active",
    suspended: "badge-status",
    archived: "badge-status text-muted-foreground",
};

// Mirrors the server validator: empty, >64 chars, or containing any of
// ".@/-" or whitespace makes the authority name ambiguous/illegal.
function validOrgName(t_name: string): string | null {
    const name = t_name.trim();
    if (!name) return "Organisation name is required";
    if (name.length > 64) return "Organisation name must be at most 64 characters";
    if (/[\s.@/-]/.test(name)) return "Name may not contain spaces, dots, @, slashes or dashes";
    return null;
}

export function OrganisationsPanel() {
    const { showEvent } = useEvent();
    const [orgs, setOrgs] = useState<OrganisationRow[]>([]);
    const [drafts, setDrafts] = useState<Record<string, OrgDraft>>({});
    const [loading, setLoading] = useState(false);
    const [savingName, setSavingName] = useState<string | null>(null);

    // Create form state
    const [creating, setCreating] = useState(false);
    const [newName, setNewName] = useState("");
    const [newDescription, setNewDescription] = useState("");
    const [newDomain, setNewDomain] = useState("");
    const [newStorageMb, setNewStorageMb] = useState("");
    const [newEntries, setNewEntries] = useState("");
    const [bootstrapAdmin, setBootstrapAdmin] = useState(false);
    const [adminEmail, setAdminEmail] = useState("");
    const [adminFirstName, setAdminFirstName] = useState("");
    const [adminFamilyName, setAdminFamilyName] = useState("");
    const [adminPassword, setAdminPassword] = useState("");

    const load = useCallback(async () => {
        setLoading(true);
        setDrafts({});
        try {
            const res = await authenticatedFetch(AdminBackendApiEndpoints.ORGANISATIONS);
            if (!res.ok) {
                throw new Error(`Failed to load organisations (HTTP ${res.status})`);
            }
            const data = await res.json();
            if (!Array.isArray(data.organisations)) {
                throw new Error("Unexpected organisation list format");
            }
            setOrgs(data.organisations as OrganisationRow[]);
        } catch (err: any) {
            console.error("Failed to load organisations:", err);
            showEvent("error", err?.message || "Unable to load organisations");
        } finally {
            setLoading(false);
        }
    }, [showEvent]);

    useEffect(() => {
        load();
    }, [load]);

    const draftFor = (t_org: OrganisationRow): OrgDraft =>
        drafts[t_org.name] ?? {
            description: t_org.description ?? "",
            domain: t_org.domain ?? "",
            status: t_org.status,
        };

    const setDraft = (t_org: OrganisationRow, t_patch: Partial<OrgDraft>) => {
        const next = { ...draftFor(t_org), ...t_patch };
        setDrafts((prev) => ({ ...prev, [t_org.name]: next }));
    };

    const handleCreate = useCallback(async () => {
        const nameError = validOrgName(newName);
        if (nameError) {
            showEvent("error", nameError);
            return;
        }
        if (bootstrapAdmin) {
            if (!adminEmail.trim() || !adminFirstName.trim() || !adminFamilyName.trim()) {
                showEvent("error", "Initial admin email, first and family name are required");
                return;
            }
            if (adminPassword.length < 12) {
                showEvent("error", "Initial admin password must be at least 12 characters");
                return;
            }
        }

        const storageBytes = mbToBytes(newStorageMb);
        if (newStorageMb.trim() && storageBytes === null) {
            showEvent("error", "Storage cap must be a non-negative MB value");
            return;
        }
        const entryLimit = newEntries.trim() === "" ? null : Number(newEntries);
        if (entryLimit !== null && (!Number.isInteger(entryLimit) || entryLimit < 0)) {
            showEvent("error", "Entry cap must be a non-negative integer");
            return;
        }

        setCreating(true);
        try {
            const body: Record<string, unknown> = {
                name: newName.trim(),
                storage_limit: storageBytes,
                entry_count_limit: entryLimit,
            };
            if (newDescription.trim()) body.description = newDescription.trim();
            if (newDomain.trim()) body.domain = newDomain.trim().toLowerCase();
            if (bootstrapAdmin) {
                body.admin = {
                    email: adminEmail.trim().toLowerCase(),
                    first_name: adminFirstName.trim(),
                    family_name: adminFamilyName.trim(),
                    password: adminPassword,
                };
            }
            const res = await authenticatedFetch(AdminBackendApiEndpoints.ORGANISATIONS, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(body),
            });
            const data = await res.json().catch(() => ({}));
            if (!res.ok) {
                throw new Error(data.message || data.error || `Failed to create organisation (HTTP ${res.status})`);
            }
            showEvent("success", `Organisation "${newName.trim()}" created`);
            setNewName("");
            setNewDescription("");
            setNewDomain("");
            setNewStorageMb("");
            setNewEntries("");
            setBootstrapAdmin(false);
            setAdminEmail("");
            setAdminFirstName("");
            setAdminFamilyName("");
            setAdminPassword("");
            await load();
        } catch (err: any) {
            showEvent("error", err?.message || "Failed to create organisation");
        } finally {
            setCreating(false);
        }
    }, [
        newName,
        newDescription,
        newDomain,
        newStorageMb,
        newEntries,
        bootstrapAdmin,
        adminEmail,
        adminFirstName,
        adminFamilyName,
        adminPassword,
        load,
        showEvent,
    ]);

    const handleSave = useCallback(
        async (t_org: OrganisationRow) => {
            const draft = draftFor(t_org);
            setSavingName(t_org.name);
            try {
                const body: Record<string, unknown> = {
                    description: draft.description.trim() ? draft.description.trim() : null,
                    domain: draft.domain.trim() ? draft.domain.trim().toLowerCase() : null,
                    status: draft.status,
                };
                const res = await authenticatedFetch(`${AdminBackendApiEndpoints.ORGANISATIONS}/${encodeURIComponent(t_org.name)}`, {
                    method: "PATCH",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(body),
                });
                const data = await res.json().catch(() => ({}));
                if (!res.ok) {
                    throw new Error(data.message || data.error || `Failed to update organisation (HTTP ${res.status})`);
                }
                showEvent("success", `Organisation "${t_org.name}" updated`);
                await load();
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to update organisation");
            } finally {
                setSavingName(null);
            }
        },
        [drafts, load, showEvent],
    );

    return (
        <div className="governance-panel">
            <div className="query-controls-row flex items-center justify-between">
                <div className="flex items-center gap-2">
                    <Building2 size={18} className="text-primary" />
                    <span className="font-semibold text-sm tracking-wider">ORGANISATION REGISTRY — {orgs.length} ROWS</span>
                </div>
                <button className="btn-desktop h-8 px-3" onClick={load} disabled={loading} title="Reload organisation registry">
                    <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
                    <span>&nbsp;RELOAD</span>
                </button>
            </div>

            <div className="roster-card">
                <div className="roster-title">CREATE ORGANISATION</div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop w-64"
                        placeholder="NAME (no spaces . @ / -) *"
                        value={newName}
                        onChange={(e) => setNewName(e.target.value)}
                        maxLength={64}
                    />
                    <input
                        className="input-desktop flex-1"
                        placeholder="DESCRIPTION"
                        value={newDescription}
                        onChange={(e) => setNewDescription(e.target.value)}
                        maxLength={256}
                    />
                    <input
                        className="input-desktop w-48"
                        placeholder="DEFAULT DOMAIN"
                        value={newDomain}
                        onChange={(e) => setNewDomain(e.target.value)}
                        maxLength={64}
                    />
                </div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop w-40"
                        type="number"
                        min={0}
                        step="any"
                        placeholder="STORAGE CAP (MB)"
                        value={newStorageMb}
                        onChange={(e) => setNewStorageMb(e.target.value)}
                    />
                    <input
                        className="input-desktop w-40"
                        type="number"
                        min={0}
                        step={1}
                        placeholder="ENTRY CAP"
                        value={newEntries}
                        onChange={(e) => setNewEntries(e.target.value)}
                    />
                    <label className="admin-config-check">
                        <input type="checkbox" checked={bootstrapAdmin} onChange={(e) => setBootstrapAdmin(e.target.checked)} />
                        <span>BOOTSTRAP INITIAL ADMIN</span>
                    </label>
                    <button className="btn-desktop-primary" onClick={handleCreate} disabled={creating}>
                        {creating ? <Loader2 className="animate-spin" size={18} /> : <Plus size={18} />}
                        <span>&nbsp;CREATE</span>
                    </button>
                </div>
                {bootstrapAdmin && (
                    <div className="query-controls-row">
                        <input
                            className="input-desktop w-64"
                            placeholder="ADMIN EMAIL *"
                            value={adminEmail}
                            onChange={(e) => setAdminEmail(e.target.value)}
                            maxLength={128}
                        />
                        <input
                            className="input-desktop w-44"
                            placeholder="ADMIN FIRST NAME *"
                            value={adminFirstName}
                            onChange={(e) => setAdminFirstName(e.target.value)}
                            maxLength={64}
                        />
                        <input
                            className="input-desktop w-44"
                            placeholder="ADMIN FAMILY NAME *"
                            value={adminFamilyName}
                            onChange={(e) => setAdminFamilyName(e.target.value)}
                            maxLength={64}
                        />
                        <input
                            className="input-desktop w-56"
                            type="password"
                            placeholder="INITIAL PASSWORD (min 12) *"
                            value={adminPassword}
                            onChange={(e) => setAdminPassword(e.target.value)}
                        />
                    </div>
                )}
            </div>

            {orgs.length > 0 ? (
                <div className="roster-card">
                    <div className="roster-title">CONFIGURE ORGANISATIONS</div>
                    <div className="roster-table-wrap">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>ORGANISATION</th>
                                    <th>DESCRIPTION</th>
                                    <th>DEFAULT DOMAIN</th>
                                    <th>STATUS</th>
                                    <th>USED / CAP</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {orgs.map((org) => {
                                    const draft = draftFor(org);
                                    return (
                                        <tr key={org.name}>
                                            <td className="admin-cell-bold">{org.name}</td>
                                            <td>
                                                <input
                                                    className="input-desktop w-56"
                                                    value={draft.description}
                                                    onChange={(e) => setDraft(org, { description: e.target.value })}
                                                    maxLength={256}
                                                    placeholder="—"
                                                />
                                            </td>
                                            <td>
                                                <input
                                                    className="input-desktop w-40 font-mono"
                                                    value={draft.domain}
                                                    onChange={(e) => setDraft(org, { domain: e.target.value })}
                                                    maxLength={64}
                                                    placeholder="—"
                                                />
                                            </td>
                                            <td>
                                                <select
                                                    className="input-desktop w-32"
                                                    value={draft.status}
                                                    onChange={(e) => setDraft(org, { status: e.target.value })}
                                                >
                                                    {STATUSES.map((s) => (
                                                        <option key={s} value={s}>
                                                            {s.toUpperCase()}
                                                        </option>
                                                    ))}
                                                </select>
                                                <span className={`${STATUS_BADGE[draft.status] ?? "badge-status"} ml-2`}>
                                                    {draft.status.toUpperCase()}
                                                </span>
                                            </td>
                                            <td className="text-xs text-muted-foreground">
                                                {bytesToMb(org.total_virtual_size)} MB /{" "}
                                                {org.storage_limit === null ? "∞" : `${bytesToMb(org.storage_limit)} MB`} ·{" "}
                                                {org.total_entry_count} / {org.entry_count_limit === null ? "∞" : org.entry_count_limit}
                                            </td>
                                            <td>
                                                <div className="admin-actions-right">
                                                    <button
                                                        className="btn-desktop h-7 px-3"
                                                        onClick={() => handleSave(org)}
                                                        disabled={savingName === org.name}
                                                        title="Save organisation configuration"
                                                    >
                                                        {savingName === org.name ? (
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
            ) : (
                <div className="governance-empty">{loading ? "LOADING ORGANISATIONS..." : "NO ORGANISATIONS FOUND"}</div>
            )}

            <div className="query-help">
                <strong>Organisation Lifecycle:</strong> creating an organisation bootstraps its default domain and (optionally) an initial
                administrator account atomically. Status transitions: <em>active</em> → <em>suspended</em> (sessions revoked, sign-ins
                rejected) → <em>archived</em>. You cannot suspend or archive your own organisation. Storage/entry caps set here apply
                organisation-wide; per-user, per-service and per-domain caps are managed in the Quotas ledger.
            </div>
        </div>
    );
}
