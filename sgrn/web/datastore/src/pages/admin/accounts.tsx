// @/pages/admin/accounts.tsx
//
// Direct configuration panels for users and domains, backed by the
// generated CRUD views (GET/POST/PATCH /api/v1/users, /api/v1/domains).
// Domain rows integrate the AES-256-GCM encryption-at-rest upload policy
// so admins configure identity and encryption in one place.
import { useState, useCallback, useEffect } from "react";
import { Loader2, Plus, Trash2, Save, Lock, ShieldCheck, RefreshCw, ChevronDown, ChevronUp, ShieldAlert } from "lucide-react";
import { UserStatus } from "@/types";
import { useEvent } from "@/contexts/EventContext";
import { authenticatedFetch } from "@/backend/api/fetcher";
import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { resetUserPassword, setUserStatus, deleteUser } from "@/backend/api/accountAdmin";

// ============================================================================
// SHARED
// ============================================================================

interface DomainRecord {
    id: number;
    name: string;
    parent_id?: number | null;
    path?: string | null;
    encryption_policy?: "none" | "optional" | "required" | string;
}

const ENCRYPTION_POLICIES = ["none", "optional", "required"] as const;
type EncryptionPolicy = (typeof ENCRYPTION_POLICIES)[number];

const ENCRYPTION_POLICY_LABELS: Record<EncryptionPolicy, string> = {
    none: "none (Plaintext only)",
    optional: "optional (Client selects)",
    required: "required (Enforce AES-256-GCM)",
};

const statusBadgeLabel = (t_status: string | null | undefined): string => (t_status || "unknown").toUpperCase();

// ============================================================================
// USER CONFIGURATION
// ============================================================================

interface UserRecord {
    id: number;
    first_name: string;
    family_name: string;
    email: string;
    phone_number?: string | null;
    role?: string | null;
    domain?: string | null;
    status?: string | null;
    is_platform_admin?: boolean;
    can_read_personal?: boolean;
    can_write_personal?: boolean;
    can_delete_personal?: boolean;
    storage_limit?: number | null;
}

interface UserDraft {
    first_name: string;
    family_name: string;
    phone_number: string;
    domain: string;
    status: string;
    can_read_personal: boolean;
    can_write_personal: boolean;
    can_delete_personal: boolean;
}

function draftForUser(t_user: UserRecord): UserDraft {
    return {
        first_name: t_user.first_name ?? "",
        family_name: t_user.family_name ?? "",
        phone_number: t_user.phone_number ?? "",
        domain: t_user.domain ?? "",
        status: t_user.status ?? UserStatus.ACTIVE,
        can_read_personal: t_user.can_read_personal ?? false,
        can_write_personal: t_user.can_write_personal ?? false,
        can_delete_personal: t_user.can_delete_personal ?? false,
    };
}

export function UserConfigPanel() {
    const { showEvent } = useEvent();
    const [users, setUsers] = useState<UserRecord[]>([]);
    const [domains, setDomains] = useState<DomainRecord[]>([]);
    const [search, setSearch] = useState("");
    const [loading, setLoading] = useState(false);
    const [editingId, setEditingId] = useState<number | null>(null);
    const [drafts, setDrafts] = useState<Record<number, UserDraft>>({});
    const [savingId, setSavingId] = useState<number | null>(null);

    const load = useCallback(async () => {
        setLoading(true);
        setEditingId(null);
        setDrafts({});
        try {
            const res = await authenticatedFetch("/api/v1/users");
            if (!res.ok) {
                throw new Error(`Failed to load user roster (HTTP ${res.status})`);
            }
            const data = await res.json();
            if (!Array.isArray(data)) {
                throw new Error("Unexpected user list format");
            }
            setUsers(data as UserRecord[]);
            const resDom = await authenticatedFetch("/api/v1/domains");
            if (resDom.ok) {
                const domData = await resDom.json();
                if (Array.isArray(domData)) {
                    setDomains(domData as DomainRecord[]);
                }
            }
        } catch (err: any) {
            console.error("Failed to load users:", err);
            showEvent("error", err?.message || "Unable to load user roster");
        } finally {
            setLoading(false);
        }
    }, [showEvent]);

    const loadDomains = useCallback(async () => {
        const res = await authenticatedFetch("/api/v1/domains");
        if (res.ok) {
            const data = await res.json();
            if (Array.isArray(data)) {
                setDomains(data as DomainRecord[]);
            }
        }
    }, []);

    useEffect(() => {
        loadDomains();
    }, [loadDomains]);

    useEffect(() => {
        load();
    }, [load]);

    const startEdit = useCallback((t_user: UserRecord) => {
        setEditingId((prev) => (prev === t_user.id ? null : t_user.id));
        setDrafts((prev) => ({ ...prev, [t_user.id]: draftForUser(t_user) }));
    }, []);

    const setDraft = useCallback((t_id: number, t_patch: Partial<UserDraft>) => {
        setDrafts((prev) => ({ ...prev, [t_id]: { ...prev[t_id], ...t_patch } }));
    }, []);

    const handleSave = useCallback(
        async (t_user: UserRecord) => {
            const draft = drafts[t_user.id];
            if (!draft) return;
            const first = draft.first_name.trim();
            const family = draft.family_name.trim();
            if (!first || !family) {
                showEvent("error", "First and family name are required");
                return;
            }
            const phone = draft.phone_number.trim();
            // Nullable columns cannot be cleared through the CRUD views
            // (phone_number is UNIQUE + format-checked, domain is a composite
            // FK with a NULL default). An emptied field therefore means
            // "leave unchanged", and a changed value must be valid.
            if (!phone && (t_user.phone_number || "").trim()) {
                showEvent("error", "Terminal number cannot be cleared — leave it unchanged or enter a valid one");
                return;
            }
            if (phone && !/^\+?[0-9]{7,15}$/.test(phone)) {
                showEvent("error", `Terminal number "${phone}" must match +<7..15 digits>`);
                return;
            }
            const oldDomain = t_user.domain ?? "";
            if (oldDomain && !draft.domain) {
                showEvent("error", "A user cannot be un-assigned from a domain (only re-assigned) — choose a domain");
                return;
            }
            setSavingId(t_user.id);
            try {
                const body: Record<string, unknown> = {};
                if (first !== (t_user.first_name ?? "")) body.first_name = first;
                if (family !== (t_user.family_name ?? "")) body.family_name = family;
                if (phone !== (t_user.phone_number ?? "")) body.phone_number = phone;
                if (draft.domain !== oldDomain) body.domain = draft.domain;
                // Status is a locked column on the generated CRUD view — it
                // MUST go through the hand-written admin route (which also
                // revokes sessions and guards the last org admin), never the
                // generic PATCH (where it would be a silent no-op).
                const statusChanged = draft.status !== (t_user.status ?? "");
                if (statusChanged && draft.status !== UserStatus.ACTIVE && draft.status !== UserStatus.SUSPENDED) {
                    showEvent("error", "Users cannot be moved back to 'invited' — choose active or suspended");
                    return;
                }
                if (draft.can_read_personal !== (t_user.can_read_personal ?? false)) body.can_read_personal = draft.can_read_personal;
                if (draft.can_write_personal !== (t_user.can_write_personal ?? false)) body.can_write_personal = draft.can_write_personal;
                if (draft.can_delete_personal !== (t_user.can_delete_personal ?? false))
                    body.can_delete_personal = draft.can_delete_personal;
                if (Object.keys(body).length === 0 && !statusChanged) {
                    showEvent("info", "No changes to save");
                    setSavingId(null);
                    return;
                }
                if (Object.keys(body).length > 0) {
                    const res = await authenticatedFetch(`/api/v1/users/${t_user.id}`, {
                        method: "PATCH",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify(body),
                    });
                    if (!res.ok) {
                        const err = await res.json().catch(() => ({}));
                        throw new Error(err.message || err.error || `Failed to update user (HTTP ${res.status})`);
                    }
                }
                if (statusChanged) {
                    const sres = await setUserStatus(t_user.id, draft.status as "active" | "suspended");
                    if (sres.error) {
                        throw new Error(sres.error);
                    }
                }
                showEvent("success", `User ${first} ${family} updated${statusChanged ? " (sessions revoked if suspended)" : ""}`);
                await load();
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to update user");
            } finally {
                setSavingId(null);
            }
        },
        [drafts, load, showEvent],
    );

    const handleResetPassword = useCallback(
        async (t_user: UserRecord) => {
            const pw = window.prompt(`New password for ${t_user.email} (min 12 characters):`);
            if (pw === null) return;
            if (pw.length < 12) {
                showEvent("error", "Password must be at least 12 characters");
                return;
            }
            setSavingId(t_user.id);
            try {
                const res = await resetUserPassword(t_user.id, pw);
                if (res.error) throw new Error(res.error);
                showEvent("success", `Password reset for ${t_user.email} — all their sessions were revoked`);
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to reset password");
            } finally {
                setSavingId(null);
            }
        },
        [showEvent],
    );

    const handleDeleteUser = useCallback(
        async (t_user: UserRecord) => {
            if (!window.confirm(`Soft-delete user ${t_user.email}? Their sessions are revoked immediately.`)) return;
            setSavingId(t_user.id);
            try {
                const res = await deleteUser(t_user.id);
                if (res.error) throw new Error(res.error);
                showEvent("success", `User ${t_user.email} deleted`);
                await load();
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to delete user");
            } finally {
                setSavingId(null);
            }
        },
        [load, showEvent],
    );

    const filtered = users.filter((u) => {
        if (!search.trim()) return true;
        const needle = search.trim().toLowerCase();
        return (
            u.email?.toLowerCase().includes(needle) ||
            `${u.first_name} ${u.family_name}`.toLowerCase().includes(needle) ||
            u.domain?.toLowerCase().includes(needle)
        );
    });

    return (
        <div className="governance-panel">
            <div className="query-controls">
                <div className="query-controls-row">
                    <input
                        className="input-desktop flex-1"
                        placeholder="SEARCH NAME, EMAIL OR DOMAIN"
                        value={search}
                        onChange={(e) => setSearch(e.target.value)}
                    />
                    <button className="btn-desktop-primary w-12" onClick={load} disabled={loading} title="Reload user roster">
                        {loading ? <Loader2 className="animate-spin" size={18} /> : <RefreshCw size={18} />}
                    </button>
                </div>
            </div>

            {filtered.length === 0 ? (
                <div className="governance-empty">{loading ? "LOADING USER ROSTER..." : "NO USERS FOUND"}</div>
            ) : (
                <div className="roster-card">
                    <div className="roster-title">
                        USER CONFIGURATION — {filtered.length} USER{filtered.length === 1 ? "" : "S"}
                    </div>
                    <div className="roster-table-wrap">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>USER</th>
                                    <th>EMAIL</th>
                                    <th>DOMAIN</th>
                                    <th>STATUS</th>
                                    <th>PERSONAL NS (R/W/D)</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {filtered.map((u) => {
                                    const isEditing = editingId === u.id;
                                    return (
                                        <UserRow
                                            key={u.id}
                                            user={u}
                                            domains={domains}
                                            isEditing={isEditing}
                                            draft={drafts[u.id]}
                                            savingId={savingId}
                                            onToggle={() => startEdit(u)}
                                            onSave={() => handleSave(u)}
                                            onResetPassword={() => handleResetPassword(u)}
                                            onDelete={() => handleDeleteUser(u)}
                                            onChange={(patch) => setDraft(u.id, patch)}
                                        />
                                    );
                                })}
                            </tbody>
                        </table>
                    </div>
                </div>
            )}

            <div className="query-help">
                <strong>Inline User Configuration</strong> edits identity (names, terminal number), assigned domain, account status and
                personal namespace permissions (read / write / delete) directly against the user record. Status changes go through the admin
                lifecycle route (sessions are revoked when a user is suspended); <em>RESET PASSWORD</em> sets a new credential and revokes
                every live session; <em>DELETE</em> soft-deletes the account. Storage quotas, max file size and rate limits are configured
                in <em>Storage Admin → Quotas</em>. Drafts apply on <em>SAVE</em> — only changed fields are written. Note: a terminal number
                (unique, <code>+&lt;7..15 digits&gt;</code>) and an assigned domain cannot be cleared, only changed, and accounts cannot be
                moved back to <em>invited</em>.
            </div>
        </div>
    );
}

/** One user renders as its main row plus — while editing — a full-width editor row. */
function UserRow(props: {
    user: UserRecord;
    domains: DomainRecord[];
    isEditing: boolean;
    draft?: UserDraft;
    savingId: number | null;
    onToggle: () => void;
    onSave: () => void;
    onResetPassword: () => void;
    onDelete: () => void;
    onChange: (t_patch: Partial<UserDraft>) => void;
}) {
    const { user, domains, isEditing, draft, savingId, onToggle, onSave, onResetPassword, onDelete, onChange } = props;
    const colSpan = 6;
    return (
        <>
            <tr>
                <td>
                    <div className="admin-cell-bold">
                        {user.first_name} {user.family_name}
                    </div>
                    {user.is_platform_admin && (
                        <div className="admin-cell-muted">
                            <ShieldAlert size={10} className="inline mr-1" />
                            platform admin
                        </div>
                    )}
                </td>
                <td>{user.email}</td>
                <td className="admin-cell-primary">{user.domain || "GLOBAL"}</td>
                <td>
                    <span
                        className={`badge-status ${user.status === UserStatus.ACTIVE ? "badge-status--active" : "text-muted-foreground"}`}
                    >
                        {statusBadgeLabel(user.status)}
                    </span>
                </td>
                <td className="text-xs font-mono">
                    {user.can_read_personal ? "R" : "-"}
                    {user.can_write_personal ? "W" : "-"}
                    {user.can_delete_personal ? "D" : "-"}
                </td>
                <td>
                    <div className="admin-actions-right">
                        <button className={`btn-desktop h-7 px-3 ${isEditing ? "btn-desktop-primary" : ""}`} onClick={onToggle}>
                            {isEditing ? <ChevronUp size={14} /> : <ChevronDown size={14} />}
                            <span>&nbsp;{isEditing ? "CLOSE" : "CONFIGURE"}</span>
                        </button>
                    </div>
                </td>
            </tr>
            {isEditing && draft && (
                <tr>
                    <td colSpan={colSpan}>
                        <div className="query-controls-row flex-wrap gap-2 py-1">
                            <input
                                className="input-desktop w-44"
                                placeholder="FIRST NAME"
                                value={draft.first_name}
                                onChange={(e) => onChange({ first_name: e.target.value })}
                                maxLength={64}
                            />
                            <input
                                className="input-desktop w-44"
                                placeholder="FAMILY NAME"
                                value={draft.family_name}
                                onChange={(e) => onChange({ family_name: e.target.value })}
                                maxLength={64}
                            />
                            <select
                                className="input-desktop w-56"
                                value={draft.domain}
                                onChange={(e) => onChange({ domain: e.target.value })}
                            >
                                <option value={""} disabled={Boolean(user.domain)}>
                                    {user.domain ? "DOMAIN (cannot clear)" : "DOMAIN: GLOBAL (none)"}
                                </option>
                                {domains.map((d) => (
                                    <option key={d.id} value={d.name}>
                                        {d.name}
                                    </option>
                                ))}
                            </select>
                            <input
                                className="input-desktop w-44"
                                placeholder="TERMINAL NUMBER (max 15 digits)"
                                value={draft.phone_number}
                                onChange={(e) => onChange({ phone_number: e.target.value })}
                                maxLength={16}
                            />
                            <select
                                className="input-desktop w-40"
                                value={draft.status}
                                onChange={(e) => onChange({ status: e.target.value })}
                            >
                                <option value={UserStatus.ACTIVE}>ACTIVE</option>
                                <option value={UserStatus.SUSPENDED}>SUSPENDED</option>
                                <option value={UserStatus.INVITED} disabled={user.status !== UserStatus.INVITED}>
                                    INVITED (REGISTRATION ONLY)
                                </option>
                            </select>
                        </div>
                        <div className="query-controls-row flex-wrap gap-2 pb-1">
                            <label className="admin-config-check">
                                <input
                                    type="checkbox"
                                    checked={draft.can_read_personal}
                                    onChange={(e) => onChange({ can_read_personal: e.target.checked })}
                                />
                                <span>READ PERSONAL NS</span>
                            </label>
                            <label className="admin-config-check">
                                <input
                                    type="checkbox"
                                    checked={draft.can_write_personal}
                                    onChange={(e) => onChange({ can_write_personal: e.target.checked })}
                                />
                                <span>WRITE PERSONAL NS</span>
                            </label>
                            <label className="admin-config-check">
                                <input
                                    type="checkbox"
                                    checked={draft.can_delete_personal}
                                    onChange={(e) => onChange({ can_delete_personal: e.target.checked })}
                                />
                                <span>DELETE PERSONAL NS</span>
                            </label>
                            <button
                                className="btn-desktop"
                                onClick={onResetPassword}
                                disabled={savingId === user.id}
                                title="Reset this user's password (revokes all their sessions)"
                            >
                                <Lock size={14} />
                                <span>&nbsp;RESET PASSWORD</span>
                            </button>
                            <button
                                className="btn-desktop"
                                onClick={onDelete}
                                disabled={savingId === user.id}
                                title="Soft-delete this user (revokes all their sessions)"
                            >
                                <Trash2 size={14} />
                                <span>&nbsp;DELETE</span>
                            </button>
                            <button
                                className="btn-desktop-primary"
                                onClick={onSave}
                                disabled={savingId === user.id}
                                title="Apply user configuration"
                            >
                                {savingId === user.id ? <Loader2 className="animate-spin" size={18} /> : <Save size={18} />}
                                <span>&nbsp;SAVE</span>
                            </button>
                        </div>
                    </td>
                </tr>
            )}
        </>
    );
}

// ============================================================================
// DOMAIN CONFIGURATION
// ============================================================================

// ============================================================================
// REGISTER USER FORM
// ============================================================================

export function RegisterUserForm() {
    const { showEvent } = useEvent();
    const [loading, setLoading] = useState(false);
    const [creating, setCreating] = useState(false);
    const [email, setEmail] = useState("");
    const [password, setPassword] = useState("");
    const [organisation, setOrganisation] = useState("");
    const [firstName, setFirstName] = useState("");
    const [familyName, setFamilyName] = useState("");

    const load = useCallback(async () => {
        const res = await authenticatedFetch("/api/v1/query/organisations");
        if (res.ok) {
            const data = await res.json();
            if (Array.isArray(data) && data.length > 0) {
                setOrganisation(data[0].name);
            }
        }
    }, []);

    useEffect(() => {
        load();
    }, [load]);

    // Initialize loading state
    useEffect(() => {
        setLoading(false);
    }, []);

    const handleSubmit = async (e: React.FormEvent) => {
        e.preventDefault();
        if (!email || !password || !organisation || !firstName || !familyName) {
            showEvent("error", "All fields are required");
            return;
        }
        setCreating(true);
        try {
            const res = await authenticatedFetch("/api/v1/admin/users/register", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ email, password, organisation, first_name: firstName, family_name: familyName }),
            });
            const data = await res.json().catch(() => ({}));
            if (res.ok) {
                showEvent("success", "User registered successfully");
                setEmail("");
                setPassword("");
                setFirstName("");
                setFamilyName("");
                await load();
            } else {
                showEvent("error", data.error || "Failed to register user");
            }
        } catch (err: any) {
            showEvent("error", err?.message || "Network error during registration");
        } finally {
            setCreating(false);
        }
    };

    return (
        <div className="reg-form-card">
            <h3>Register New User</h3>
            <form onSubmit={handleSubmit} className="reg-form">
                <div className="reg-input-group">
                    <input
                        className="input-desktop"
                        placeholder="EMAIL *"
                        value={email}
                        onChange={(e) => setEmail(e.target.value)}
                        required
                    />
                    <input
                        className="input-desktop"
                        placeholder="PASSWORD (min 12 chars) *"
                        type="password"
                        value={password}
                        onChange={(e) => setPassword(e.target.value)}
                        required
                    />
                </div>
                <div className="reg-input-group">
                    <input
                        className="input-desktop"
                        placeholder="ORGANISATION *"
                        value={organisation}
                        onChange={(e) => setOrganisation(e.target.value)}
                        required
                    />
                    <input
                        className="input-desktop"
                        placeholder="FIRST NAME *"
                        value={firstName}
                        onChange={(e) => setFirstName(e.target.value)}
                        required
                    />
                    <input
                        className="input-desktop"
                        placeholder="FAMILY NAME *"
                        value={familyName}
                        onChange={(e) => setFamilyName(e.target.value)}
                        required
                    />
                </div>
                <button className="btn-desktop-primary" type="submit" disabled={creating || loading}>
                    {creating ? <Loader2 className="animate-spin" size={18} /> : <Plus size={18} />}
                    <span>&nbsp;REGISTER</span>
                </button>
            </form>
        </div>
    );
}

export function DomainConfigPanel() {
    const { showEvent } = useEvent();
    const [domains, setDomains] = useState<DomainRecord[]>([]);
    const [drafts, setDrafts] = useState<Record<number, DomainDraft>>({});
    const [loading, setLoading] = useState(false);
    const [savingId, setSavingId] = useState<number | null>(null);
    const [creating, setCreating] = useState(false);
    const [armingDelete, setArmingDelete] = useState<number | null>(null);

    const [newName, setNewName] = useState("");
    const [newParent, setNewParent] = useState("");
    const [newPolicy, setNewPolicy] = useState<EncryptionPolicy>("optional");

    const load = useCallback(async () => {
        setLoading(true);
        setDrafts({});
        setArmingDelete(null);
        try {
            const res = await authenticatedFetch("/api/v1/domains");
            if (!res.ok) {
                throw new Error(`Failed to load domains (HTTP ${res.status})`);
            }
            const data = await res.json();
            if (!Array.isArray(data)) {
                throw new Error("Unexpected domain list format");
            }
            setDomains(data as DomainRecord[]);
        } catch (err: any) {
            console.error("Failed to load domains:", err);
            showEvent("error", err?.message || "Unable to load domain registry");
        } finally {
            setLoading(false);
        }
    }, [showEvent]);

    useEffect(() => {
        load();
    }, [load]);

    const handleCreate = useCallback(async () => {
        if (!newName.trim()) {
            showEvent("error", "Domain name is required");
            return;
        }
        if (domains.some((d) => d.name.toLowerCase() === newName.trim().toLowerCase())) {
            showEvent("error", `Domain "${newName.trim()}" already exists`);
            return;
        }
        setCreating(true);
        try {
            const body: Record<string, unknown> = { name: newName.trim(), encryption_policy: newPolicy };
            if (newParent) {
                body.parent_id = Number(newParent);
            }
            const res = await authenticatedFetch("/api/v1/domains", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(body),
            });
            if (!res.ok) {
                const err = await res.json().catch(() => ({}));
                throw new Error(err.message || err.error || `Failed to create domain (HTTP ${res.status})`);
            }
            showEvent("success", `Domain "${newName.trim()}" registered with encryption policy: ${newPolicy}`);
            setNewName("");
            setNewParent("");
            setNewPolicy("optional");
            await load();
        } catch (err: any) {
            showEvent("error", err?.message || "Failed to create domain");
        } finally {
            setCreating(false);
        }
    }, [newName, newParent, newPolicy, domains, load, showEvent]);

    const handleSave = useCallback(
        async (t_domain: DomainRecord) => {
            const draft = drafts[t_domain.id];
            if (!draft) return;
            setSavingId(t_domain.id);
            try {
                // Name and parent changes go through the hand-written
                // lifecycle routes (admin-domains.cpp): rename re-labels every
                // authority in the sub-tree, move supports re-parenting to
                // root (the generated CRUD view cannot clear a NULL column).
                // Only the encryption policy rides the generic CRUD PATCH.
                const nameChanged = draft.name.trim() !== (t_domain.name ?? "");
                const parentChanged = String(draft.parent_id ?? "null") !== String(t_domain.parent_id ?? "null");
                const policyChanged = draft.encryption_policy !== (t_domain.encryption_policy || "optional");
                if (!nameChanged && !parentChanged && !policyChanged) {
                    showEvent("info", "No changes to save");
                    setSavingId(null);
                    return;
                }
                if (nameChanged) {
                    if (!draft.name.trim()) {
                        throw new Error("Domain name must not be empty");
                    }
                    const rres = await authenticatedFetch(AdminBackendApiEndpoints.DOMAIN_RENAME(t_domain.id), {
                        method: "POST",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({ name: draft.name.trim() }),
                    });
                    if (!rres.ok) {
                        const err = await rres.json().catch(() => ({}));
                        throw new Error(err.message || err.error || `Failed to rename domain (HTTP ${rres.status})`);
                    }
                }
                if (parentChanged) {
                    const mres = await authenticatedFetch(AdminBackendApiEndpoints.DOMAIN_MOVE(t_domain.id), {
                        method: "POST",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({ parent_id: draft.parent_id }),
                    });
                    if (!mres.ok) {
                        const err = await mres.json().catch(() => ({}));
                        throw new Error(err.message || err.error || `Failed to move domain (HTTP ${mres.status})`);
                    }
                }
                if (policyChanged) {
                    const res = await authenticatedFetch(`/api/v1/domains/${t_domain.id}`, {
                        method: "PATCH",
                        headers: { "Content-Type": "application/json" },
                        body: JSON.stringify({ encryption_policy: draft.encryption_policy }),
                    });
                    if (!res.ok) {
                        const err = await res.json().catch(() => ({}));
                        throw new Error(err.message || err.error || `Failed to update domain (HTTP ${res.status})`);
                    }
                }
                showEvent("success", `Domain "${draft.name.trim()}" updated${nameChanged ? " (authorities re-labelled)" : ""}`);
                await load();
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to update domain");
            } finally {
                setSavingId(null);
            }
        },
        [drafts, load, showEvent],
    );

    const handleDelete = useCallback(
        async (t_domain: DomainRecord) => {
            if (armingDelete !== t_domain.id) {
                setArmingDelete(t_domain.id);
                return;
            }
            setArmingDelete(null);
            try {
                const res = await authenticatedFetch(`/api/v1/domains/${t_domain.id}`, { method: "DELETE" });
                if (!res.ok && res.status !== 204) {
                    const err = await res.json().catch(() => ({}));
                    throw new Error(err.message || err.error || `Failed to delete domain (HTTP ${res.status})`);
                }
                showEvent("success", `Domain "${t_domain.name}" deleted`);
                await load();
            } catch (err: any) {
                showEvent("error", err?.message || "Failed to delete domain");
            }
        },
        [armingDelete, load, showEvent],
    );

    const draftFor = (t_domain: DomainRecord): DomainDraft =>
        drafts[t_domain.id] ?? {
            name: t_domain.name ?? "",
            parent_id: t_domain.parent_id ?? null,
            encryption_policy: (t_domain.encryption_policy || "optional") as EncryptionPolicy,
        };

    const setDraft = (t_domain: DomainRecord, t_patch: Partial<DomainDraft>) => {
        const next = { ...draftFor(t_domain), ...t_patch };
        setDrafts((prev) => ({ ...prev, [t_domain.id]: next }));
    };

    const hasChildren = (t_id: number): boolean => domains.some((d) => d.parent_id === t_id);

    return (
        <div className="governance-panel">
            <div className="query-controls-row flex items-center justify-between">
                <div className="flex items-center gap-2">
                    <ShieldCheck size={18} className="text-primary" />
                    <span className="font-semibold text-sm tracking-wider">DOMAIN REGISTRY — {domains.length} DOMAINS</span>
                </div>
                <button className="btn-desktop h-8 px-3" onClick={load} disabled={loading} title="Reload domain registry">
                    <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
                    <span>&nbsp;RELOAD</span>
                </button>
            </div>

            <div className="roster-card">
                <div className="roster-title">REGISTER DOMAIN</div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop w-64"
                        placeholder="DOMAIN NAME *"
                        value={newName}
                        onChange={(e) => setNewName(e.target.value)}
                        maxLength={64}
                    />
                    <select className="input-desktop w-56" value={newParent} onChange={(e) => setNewParent(e.target.value)}>
                        <option value="">PARENT DOMAIN: (root)</option>
                        {domains.map((d) => (
                            <option key={d.id} value={d.id}>
                                {d.name} — {d.path || "/"}
                            </option>
                        ))}
                    </select>
                    <select
                        className="input-desktop w-56"
                        value={newPolicy}
                        onChange={(e) => setNewPolicy(e.target.value as EncryptionPolicy)}
                    >
                        {ENCRYPTION_POLICIES.map((p) => (
                            <option key={p} value={p}>
                                {ENCRYPTION_POLICY_LABELS[p]}
                            </option>
                        ))}
                    </select>
                    <button className="btn-desktop-primary" onClick={handleCreate} disabled={creating}>
                        {creating ? <Loader2 className="animate-spin" size={18} /> : <Plus size={18} />}
                        <span>&nbsp;CREATE</span>
                    </button>
                </div>
            </div>

            {domains.length > 0 ? (
                <div className="roster-card">
                    <div className="roster-title">CONFIGURE DOMAINS</div>
                    <div className="roster-table-wrap">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>DOMAIN</th>
                                    <th>PATH</th>
                                    <th>PARENT</th>
                                    <th>ENCRYPTION POLICY</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {domains.map((d) => {
                                    const draft = draftFor(d);
                                    const children = hasChildren(d.id);
                                    return (
                                        <tr key={d.id}>
                                            <td>
                                                <input
                                                    className="input-desktop w-44"
                                                    value={draft.name}
                                                    onChange={(e) => setDraft(d, { name: e.target.value })}
                                                    maxLength={64}
                                                />
                                            </td>
                                            <td className="font-mono text-xs text-muted-foreground">{d.path || "/"}</td>
                                            <td>
                                                <select
                                                    className="input-desktop w-48"
                                                    value={draft.parent_id === null ? "" : String(draft.parent_id)}
                                                    onChange={(e) =>
                                                        setDraft(d, { parent_id: e.target.value ? Number(e.target.value) : null })
                                                    }
                                                >
                                                    {/* Re-parenting runs through the lifecycle MOVE
                                                        route, which supports returning to root. */}
                                                    <option value="">
                                                        {d.parent_id == null ? "(root — current value)" : "(root — top level)"}
                                                    </option>
                                                    {domains
                                                        .filter((other) => other.id !== d.id)
                                                        .map((other) => (
                                                            <option key={other.id} value={other.id}>
                                                                {other.name}
                                                            </option>
                                                        ))}
                                                </select>
                                            </td>
                                            <td>
                                                <select
                                                    className={`input-desktop w-56 ${draft.encryption_policy === "required" ? "text-primary" : ""}`}
                                                    value={draft.encryption_policy}
                                                    onChange={(e) => setDraft(d, { encryption_policy: e.target.value as EncryptionPolicy })}
                                                >
                                                    {ENCRYPTION_POLICIES.map((p) => (
                                                        <option key={p} value={p}>
                                                            {ENCRYPTION_POLICY_LABELS[p]}
                                                        </option>
                                                    ))}
                                                </select>
                                            </td>
                                            <td>
                                                <div className="admin-actions-right">
                                                    {draft.encryption_policy === "required" && (
                                                        <span
                                                            className="badge-status badge-status--active"
                                                            title="Uploads enforce encryption-at-rest"
                                                        >
                                                            <Lock size={10} className="inline mr-1" />
                                                            ENFORCE AES-256-GCM
                                                        </span>
                                                    )}
                                                    <button
                                                        className="btn-desktop h-7 px-3"
                                                        onClick={() => handleSave(d)}
                                                        disabled={savingId === d.id}
                                                        title="Save domain configuration"
                                                    >
                                                        {savingId === d.id ? (
                                                            <Loader2 className="animate-spin" size={14} />
                                                        ) : (
                                                            <Save size={14} />
                                                        )}
                                                        <span>&nbsp;SAVE</span>
                                                    </button>
                                                    <button
                                                        className={`btn-desktop h-7 px-3 ${armingDelete === d.id ? "btn-desktop-primary" : ""}`}
                                                        onClick={() => handleDelete(d)}
                                                        disabled={children}
                                                        title={children ? "Delete or reassign sub-domains first" : "Delete domain"}
                                                    >
                                                        {armingDelete === d.id ? "CONFIRM" : <Trash2 size={14} />}
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
                <div className="governance-empty">{loading ? "LOADING DOMAIN REGISTRY..." : "NO DOMAINS FOUND"}</div>
            )}

            <div className="query-help">
                <strong>Domain Configuration</strong> covers hierarchy (parent domain — the path is recomputed automatically), name, and the{" "}
                <strong>encryption upload policy</strong>:
                <ul className="list-disc pl-5 mt-1 space-y-1">
                    <li>
                        <strong>none:</strong> uploads are stored in plaintext; encryption requests are ignored.
                    </li>
                    <li>
                        <strong>optional:</strong> uploads are encrypted (AES-256-GCM) when the uploader toggles encryption in the Drive
                        toolbar.
                    </li>
                    <li>
                        <strong>required:</strong> every upload is strictly encrypted with AES-256-GCM; plaintext uploads are rejected.
                    </li>
                </ul>
                <span className="mt-1 block">
                    Delete is disabled while sub-domains reference the parent. Renaming re-labels every authority under the sub-tree and
                    re-parenting (including back to root) goes through the lifecycle <code>rename</code>/<code>move</code> routes.
                </span>
            </div>
        </div>
    );
}

interface DomainDraft {
    name: string;
    parent_id: number | null;
    encryption_policy: EncryptionPolicy;
}
