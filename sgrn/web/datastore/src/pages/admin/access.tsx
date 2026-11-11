import { useState, useCallback, useEffect } from "react";
import { Loader2, Search, Plus, Trash2, Shield, RefreshCw, Save, UserPlus, UserMinus } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import { authenticatedFetch } from "@/backend/api/fetcher";
import { fetchPermissions, grantPermission, revokePermission, type DomainPermission } from "@/backend/api/permissions";
import {
    fetchRoles,
    fetchRoleAssignments,
    createRole,
    updateRole,
    deleteRole,
    assignRole,
    revokeRole,
    type RoleEntry,
    type RoleAssignment,
} from "@/backend/api/roles";

// Directory entries for role assignment targets (org-scoped lists from the
// generated CRUD views).
interface AssignTarget {
    id: number;
    label: string;
}

export default function PermissionsPanel() {
    const { showEvent } = useEvent();
    const [rows, setRows] = useState<DomainPermission[]>([]);
    const [searchEmail, setSearchEmail] = useState("");
    const [searchDomain, setSearchDomain] = useState("");
    const [loading, setLoading] = useState(false);
    const [saving, setSaving] = useState(false);
    const [armingDelete, setArmingDelete] = useState<number | null>(null);

    const [grantId, setGrantId] = useState("");
    const [grantDomain, setGrantDomain] = useState("");
    const [grantSubpath, setGrantSubpath] = useState("/");
    const [grantRead, setGrantRead] = useState(true);
    const [grantWrite, setGrantWrite] = useState(true);
    const [grantDelete, setGrantDelete] = useState(false);

    // Roles State
    const [roles, setRoles] = useState<RoleEntry[]>([]);
    const [assignments, setAssignments] = useState<RoleAssignment[]>([]);
    const [loadingRoles, setLoadingRoles] = useState(false);
    const [creatingRole, setCreatingRole] = useState(false);
    const [newRoleName, setNewRoleName] = useState("");
    const [newRoleDesc, setNewRoleDesc] = useState("");
    const [newRolePerms, setNewRolePerms] = useState("");

    // Role editing / assignment state. Edits apply per role id; assignment
    // targets are loaded from the org-scoped user/service lists.
    const [roleEdits, setRoleEdits] = useState<Record<number, { description: string; perms: string }>>({});
    const [busyRoleId, setBusyRoleId] = useState<number | null>(null);
    const [assignTargetKind, setAssignTargetKind] = useState<"user" | "service">("user");
    const [assignTargets, setAssignTargets] = useState<AssignTarget[]>([]);
    const [assignTargetId, setAssignTargetId] = useState("");
    const [assignRoleId, setAssignRoleId] = useState("");
    const [assigning, setAssigning] = useState(false);

    const roleEditFor = useCallback(
        (t_role: RoleEntry): { description: string; perms: string } =>
            roleEdits[t_role.id] ?? {
                description: t_role.description || "",
                perms: Array.isArray(t_role.permissions) ? t_role.permissions.join(", ") : "",
            },
        [roleEdits],
    );

    const setRoleEdit = useCallback((t_role: RoleEntry, t_patch: Partial<{ description: string; perms: string }>) => {
        setRoleEdits((prev) => ({
            ...prev,
            [t_role.id]: {
                ...(prev[t_role.id] ?? {
                    description: t_role.description || "",
                    perms: Array.isArray(t_role.permissions) ? t_role.permissions.join(", ") : "",
                }),
                ...t_patch,
            },
        }));
    }, []);

    const loadAssignTargets = useCallback(async (t_kind: "user" | "service") => {
        setAssignTargets([]);
        setAssignTargetId("");
        try {
            // Org admins get their own org's rows; platform admins may pass
            // ?organisation= — the default list is scoped server-side.
            const res = await authenticatedFetch(t_kind === "user" ? "/api/v1/users" : "/api/v1/automated-services");
            if (!res.ok) return;
            const data = await res.json();
            if (!Array.isArray(data)) return;
            const parsed: AssignTarget[] = data.map((entry: Record<string, unknown>) =>
                t_kind === "user"
                    ? { id: Number(entry.id) || 0, label: String(entry.email || `user #${entry.id}`) }
                    : { id: Number(entry.id) || 0, label: String(entry.name || `service #${entry.id}`) },
            );
            setAssignTargets(parsed.filter((t) => t.id > 0));
        } catch {
            // directory is best-effort; assignment still works by typing an id
        }
    }, []);

    const loadRoles = useCallback(async () => {
        setLoadingRoles(true);
        try {
            const result = await fetchRoles();
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load roles");
            }
            setRoles(result.data.roles);
            const aresult = await fetchRoleAssignments();
            if (!aresult.error && aresult.data) {
                setAssignments(aresult.data.assignments);
            }
        } catch (error) {
            console.error("Failed to fetch roles:", error);
            showEvent("error", "Unable to load dynamic roles");
        } finally {
            setLoadingRoles(false);
        }
    }, [showEvent]);

    const handleUpdateRole = useCallback(
        async (t_role: RoleEntry) => {
            const edit = roleEdits[t_role.id];
            if (!edit) return;
            const permsArray = edit.perms
                .split(",")
                .map((p) => p.trim())
                .filter(Boolean);
            const descriptionChanged = edit.description !== (t_role.description || "");
            const permsChanged = JSON.stringify(permsArray) !== JSON.stringify(Array.isArray(t_role.permissions) ? t_role.permissions : []);
            if (!descriptionChanged && !permsChanged) {
                showEvent("info", "No role changes to save");
                return;
            }
            setBusyRoleId(t_role.id);
            try {
                const body: Record<string, unknown> = {};
                if (descriptionChanged) body.description = edit.description;
                if (permsChanged) body.permissions = permsArray;
                const result = await updateRole(t_role.id, body);
                if (result.error || !result.data) {
                    throw new Error(result.error || "Failed to update role");
                }
                showEvent("success", `Role '${t_role.name}' updated`);
                setRoleEdits((prev) => {
                    const next = { ...prev };
                    delete next[t_role.id];
                    return next;
                });
                await loadRoles();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : "Failed to update role");
            } finally {
                setBusyRoleId(null);
            }
        },
        [roleEdits, loadRoles, showEvent],
    );

    const handleDeleteRole = useCallback(
        async (t_role: RoleEntry) => {
            if (t_role.is_system) {
                showEvent("error", "System roles cannot be deleted");
                return;
            }
            if (!window.confirm(`Delete role '${t_role.name}'? Assignments referencing it are removed.`)) return;
            setBusyRoleId(t_role.id);
            try {
                const result = await deleteRole(t_role.id);
                if (result.error || !result.data) {
                    throw new Error(result.error || "Failed to delete role");
                }
                showEvent("success", `Role '${t_role.name}' deleted`);
                await loadRoles();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : "Failed to delete role");
            } finally {
                setBusyRoleId(null);
            }
        },
        [loadRoles, showEvent],
    );

    const handleAssignRevokeRole = useCallback(
        async (t_revoke: boolean) => {
            const roleId = Number(assignRoleId);
            const targetId = Number(assignTargetId);
            if (!roleId || !targetId) {
                showEvent("error", "Select both a target and a role");
                return;
            }
            setAssigning(true);
            try {
                const payload =
                    assignTargetKind === "user"
                        ? { role_id: roleId, user_id: targetId }
                        : { role_id: roleId, automated_service_id: targetId };
                const result = t_revoke ? await revokeRole(payload) : await assignRole(payload);
                if (result.error || !result.data) {
                    throw new Error(result.error || (t_revoke ? "Failed to revoke role" : "Failed to assign role"));
                }
                showEvent(
                    "success",
                    `${t_revoke ? "Revoked" : "Assigned"} role #${roleId} ${t_revoke ? "from" : "to"} ${
                        assignTargetKind === "user" ? "user" : "service"
                    } #${targetId}`,
                );
                await loadRoles();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : t_revoke ? "Failed to revoke role" : "Failed to assign role");
            } finally {
                setAssigning(false);
            }
        },
        [assignRoleId, assignTargetId, assignTargetKind, loadRoles, showEvent],
    );

    const handleCreateRole = useCallback(async () => {
        if (!newRoleName.trim()) {
            showEvent("error", "Role name is required");
            return;
        }
        setCreatingRole(true);
        try {
            const permsArray = newRolePerms
                .split(",")
                .map((p) => p.trim())
                .filter(Boolean);
            const result = await createRole({
                name: newRoleName.trim(),
                description: newRoleDesc.trim(),
                permissions: permsArray,
            });
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to create role");
            }
            showEvent("success", `Custom role '${newRoleName.trim()}' created successfully`);
            setNewRoleName("");
            setNewRoleDesc("");
            setNewRolePerms("");
            await loadRoles();
        } catch (error) {
            showEvent("error", error instanceof Error ? error.message : "Failed to create role");
        } finally {
            setCreatingRole(false);
        }
    }, [newRoleName, newRoleDesc, newRolePerms, loadRoles, showEvent]);

    const load = useCallback(async () => {
        setLoading(true);
        try {
            const result = await fetchPermissions({ email: searchEmail, domain: searchDomain });
            if (result.error || !result.data) {
                throw new Error(result.error || "Failed to load permissions");
            }
            setRows(result.data.permissions);
        } catch (error) {
            console.error("Failed to fetch permissions:", error);
            showEvent("error", "Unable to load domain permissions");
        } finally {
            setLoading(false);
        }
    }, [searchEmail, searchDomain, showEvent]);

    const handleGrant = useCallback(async () => {
        if (!grantId.trim() || !grantDomain.trim()) {
            showEvent("error", "User (id or email) and domain are required");
            return;
        }
        setSaving(true);
        try {
            const idNum = Number(grantId.trim());
            const body: {
                user_id?: number;
                email?: string;
                domain: string;
                allowed_subpath: string;
                can_read: boolean;
                can_write: boolean;
                can_delete: boolean;
            } = {
                domain: grantDomain.trim(),
                allowed_subpath: grantSubpath.trim() || "/",
                can_read: grantRead,
                can_write: grantWrite,
                can_delete: grantDelete,
            };
            if (Number.isInteger(idNum) && idNum > 0 && String(idNum) === grantId.trim()) {
                body.user_id = idNum;
            } else {
                body.email = grantId.trim();
            }
            const result = await grantPermission(body);
            if (result.error || !result.data) {
                throw new Error(result.error || "Grant failed");
            }
            showEvent("success", `Access granted: ${result.data.email} → ${result.data.domain}`);
            if (result.data.note) {
                showEvent("warning", result.data.note);
            }
            setGrantId("");
            setGrantDomain("");
            setGrantSubpath("/");
            await load();
        } catch (error) {
            showEvent("error", error instanceof Error ? error.message : "Grant failed");
        } finally {
            setSaving(false);
        }
    }, [grantId, grantDomain, grantSubpath, grantRead, grantWrite, grantDelete, load, showEvent]);

    const handleRevoke = useCallback(
        async (t_row: DomainPermission) => {
            if (armingDelete !== t_row.id) {
                setArmingDelete(t_row.id);
                return;
            }
            setArmingDelete(null);
            try {
                const result = await revokePermission({ id: t_row.id });
                if (result.error || !result.data) {
                    throw new Error(result.error || "Revoke failed");
                }
                showEvent("success", `Revoked: ${t_row.email} → ${t_row.domain}`);
                if (result.data.warning) {
                    showEvent("warning", result.data.warning);
                }
                await load();
            } catch (error) {
                showEvent("error", error instanceof Error ? error.message : "Revoke failed");
            }
        },
        [armingDelete, load, showEvent],
    );

    useEffect(() => {
        load();
        loadRoles();
        loadAssignTargets("user");
    }, [load, loadRoles, loadAssignTargets]);

    return (
        <div className="query-builder">
            <div className="query-controls">
                <div className="query-controls-row">
                    <input
                        className="input-desktop flex-1"
                        placeholder="SEARCH EMAIL"
                        value={searchEmail}
                        onChange={(e) => setSearchEmail(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === "Enter") load();
                        }}
                    />
                    <input
                        className="input-desktop w-64"
                        placeholder="DOMAIN"
                        value={searchDomain}
                        onChange={(e) => setSearchDomain(e.target.value)}
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
                    <div className="roster-title flex items-center justify-between">
                        <span>
                            DOMAIN GRANTS — {rows.length} ROW{rows.length === 1 ? "" : "S"}
                        </span>
                        <button
                            className="btn-desktop-secondary text-xs px-2 py-1 flex items-center gap-1"
                            onClick={load}
                            disabled={loading}
                        >
                            <RefreshCw size={12} className={loading ? "animate-spin" : ""} /> REFRESH
                        </button>
                    </div>
                    <div className="datagrid-wrapper">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>USER</th>
                                    <th>DOMAIN</th>
                                    <th>SUBPATH</th>
                                    <th>CAPABILITIES</th>
                                    <th className="text-right">OPERATIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {rows.map((r) => (
                                    <tr key={r.id}>
                                        <td>
                                            <div className="admin-cell-bold">{r.email}</div>
                                            <div className="admin-cell-muted">
                                                #{r.user_id} · {r.organisation}
                                            </div>
                                        </td>
                                        <td className="admin-cell-primary">{r.domain}</td>
                                        <td>
                                            <code>{r.allowed_subpath}</code>
                                        </td>
                                        <td>
                                            <div style={{ display: "flex", gap: "0.4rem", alignItems: "center" }}>
                                                <span
                                                    style={{
                                                        padding: "2px 8px",
                                                        borderRadius: "4px",
                                                        fontSize: "0.7rem",
                                                        fontWeight: 600,
                                                        backgroundColor: r.can_read
                                                            ? "rgba(16, 185, 129, 0.15)"
                                                            : "rgba(107, 114, 128, 0.15)",
                                                        color: r.can_read ? "#10b981" : "#6b7280",
                                                        border: `1px solid ${r.can_read ? "rgba(16, 185, 129, 0.3)" : "rgba(107, 114, 128, 0.2)"}`,
                                                    }}
                                                >
                                                    READ
                                                </span>
                                                <span
                                                    style={{
                                                        padding: "2px 8px",
                                                        borderRadius: "4px",
                                                        fontSize: "0.7rem",
                                                        fontWeight: 600,
                                                        backgroundColor: r.can_write
                                                            ? "rgba(59, 130, 246, 0.15)"
                                                            : "rgba(107, 114, 128, 0.15)",
                                                        color: r.can_write ? "#60a5fa" : "#6b7280",
                                                        border: `1px solid ${r.can_write ? "rgba(59, 130, 246, 0.3)" : "rgba(107, 114, 128, 0.2)"}`,
                                                    }}
                                                >
                                                    WRITE
                                                </span>
                                                <span
                                                    style={{
                                                        padding: "2px 8px",
                                                        borderRadius: "4px",
                                                        fontSize: "0.7rem",
                                                        fontWeight: 600,
                                                        backgroundColor: r.can_delete
                                                            ? "rgba(244, 63, 94, 0.15)"
                                                            : "rgba(107, 114, 128, 0.15)",
                                                        color: r.can_delete ? "#fb7185" : "#6b7280",
                                                        border: `1px solid ${r.can_delete ? "rgba(244, 63, 94, 0.3)" : "rgba(107, 114, 128, 0.2)"}`,
                                                    }}
                                                >
                                                    DELETE
                                                </span>
                                            </div>
                                        </td>
                                        <td>
                                            <div className="admin-actions-right">
                                                <button
                                                    className={`btn-desktop h-7 px-3 ${armingDelete === r.id ? "btn-desktop-primary" : ""}`}
                                                    style={
                                                        armingDelete === r.id ? { backgroundColor: "#dc2626", borderColor: "#b91c1c" } : {}
                                                    }
                                                    onClick={() => handleRevoke(r)}
                                                >
                                                    {armingDelete === r.id ? "CONFIRM REVOKE" : <Trash2 size={14} />}
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
                <div className="roster-title flex items-center gap-2">
                    <Plus size={16} /> GRANT DOMAIN ACCESS
                </div>
                <div className="query-controls-row">
                    <input
                        className="input-desktop w-64"
                        placeholder="USER ID OR EMAIL"
                        value={grantId}
                        onChange={(e) => setGrantId(e.target.value)}
                    />
                    <input
                        className="input-desktop w-64"
                        placeholder="DOMAIN"
                        value={grantDomain}
                        onChange={(e) => setGrantDomain(e.target.value)}
                    />
                    <input
                        className="input-desktop w-64"
                        placeholder="SUBPATH (default /)"
                        value={grantSubpath}
                        onChange={(e) => setGrantSubpath(e.target.value)}
                    />
                    <label className="admin-config-check">
                        <input type="checkbox" checked={grantRead} onChange={(e) => setGrantRead(e.target.checked)} />
                        <span>READ</span>
                    </label>
                    <label className="admin-config-check">
                        <input type="checkbox" checked={grantWrite} onChange={(e) => setGrantWrite(e.target.checked)} />
                        <span>WRITE</span>
                    </label>
                    <label className="admin-config-check">
                        <input type="checkbox" checked={grantDelete} onChange={(e) => setGrantDelete(e.target.checked)} />
                        <span>DELETE</span>
                    </label>
                    <button className="btn-desktop-primary" onClick={handleGrant} disabled={saving}>
                        {saving ? <Loader2 className="animate-spin" size={18} /> : <Plus size={18} />}
                        <span>&nbsp;GRANT</span>
                    </button>
                </div>
            </div>

            <div className="query-help">
                <strong>Zero-trust:</strong> a user with no grant row for a domain is denied outright — granting the first row is what
                unlocks them, revoking the last one locks them out again. Subpath sandboxes them inside one tree; the domain must already
                exist in the user's organisation.
            </div>

            <div className="roster-card" style={{ marginTop: "2rem" }}>
                <div className="roster-title flex items-center justify-between">
                    <span className="flex items-center gap-2">
                        <Shield size={16} /> DYNAMIC RBAC ROLES & PERMISSIONS MATRIX
                    </span>
                    <button className="btn-desktop-primary" onClick={loadRoles} disabled={loadingRoles}>
                        {loadingRoles ? <Loader2 className="animate-spin" size={18} /> : <RefreshCw size={14} />}
                        <span>&nbsp;REFRESH ROLES</span>
                    </button>
                </div>

                {roles.length > 0 && (
                    <div className="datagrid-wrapper" style={{ marginTop: "1rem" }}>
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>ID</th>
                                    <th>NAME</th>
                                    <th>DESCRIPTION</th>
                                    <th>TYPE</th>
                                    <th>PERMISSIONS</th>
                                </tr>
                            </thead>
                            <tbody>
                                {roles.map((r) => (
                                    <tr key={r.id}>
                                        <td>#{r.id}</td>
                                        <td className="admin-cell-bold">{r.name}</td>
                                        <td>
                                            <input
                                                className="input-desktop w-44"
                                                value={roleEditFor(r).description}
                                                placeholder="DESCRIPTION"
                                                onChange={(e) => setRoleEdit(r, { description: e.target.value })}
                                            />
                                        </td>
                                        <td>
                                            <span
                                                style={{
                                                    padding: "2px 8px",
                                                    borderRadius: "4px",
                                                    fontSize: "0.7rem",
                                                    fontWeight: 700,
                                                    backgroundColor: r.is_system ? "rgba(147, 51, 234, 0.15)" : "rgba(6, 182, 212, 0.15)",
                                                    color: r.is_system ? "#c084fc" : "#22d3ee",
                                                    border: `1px solid ${r.is_system ? "rgba(147, 51, 234, 0.3)" : "rgba(6, 182, 212, 0.3)"}`,
                                                }}
                                            >
                                                {r.is_system ? "SYSTEM" : "CUSTOM"}
                                            </span>
                                        </td>
                                        <td>
                                            <div style={{ display: "flex", alignItems: "flex-start", gap: "0.4rem" }}>
                                                <input
                                                    className="input-desktop font-mono"
                                                    style={{ minWidth: 220 }}
                                                    value={roleEditFor(r).perms}
                                                    placeholder="storage:read, storage:write"
                                                    onChange={(e) => setRoleEdit(r, { perms: e.target.value })}
                                                />
                                                <button
                                                    className="btn-desktop h-7 px-3"
                                                    onClick={() => handleUpdateRole(r)}
                                                    disabled={busyRoleId === r.id || r.is_system}
                                                    title={r.is_system ? "System roles are read-only" : "Save description / permissions"}
                                                >
                                                    {busyRoleId === r.id ? (
                                                        <Loader2 className="animate-spin" size={14} />
                                                    ) : (
                                                        <Save size={14} />
                                                    )}
                                                </button>
                                                <button
                                                    className="btn-desktop h-7 px-3"
                                                    onClick={() => handleDeleteRole(r)}
                                                    disabled={busyRoleId === r.id || r.is_system}
                                                    title={r.is_system ? "System roles cannot be deleted" : "Delete this custom role"}
                                                >
                                                    <Trash2 size={14} />
                                                </button>
                                            </div>
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                )}

                {assignments.length > 0 && (
                    <div className="datagrid-wrapper" style={{ marginTop: "1rem" }}>
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th colSpan={3} style={{ textAlign: "left" }}>
                                        CURRENT ROLE ASSIGNMENTS — {assignments.length}
                                    </th>
                                </tr>
                                <tr>
                                    <th>ROLE</th>
                                    <th>TARGET</th>
                                    <th>TYPE</th>
                                </tr>
                            </thead>
                            <tbody>
                                {assignments.map((a, idx) => (
                                    <tr key={`${a.role_id}-${a.target_type}-${a.target_id}-${idx}`}>
                                        <td className="admin-cell-bold">
                                            #{a.role_id} {a.role_name}
                                        </td>
                                        <td className="font-mono">{a.target_label}</td>
                                        <td>
                                            <span className="badge-status">{a.target_type === "user" ? "USER" : "SERVICE"}</span>
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    </div>
                )}

                <div className="query-controls-row" style={{ marginTop: "1.5rem" }}>
                    <select
                        className="input-desktop w-32"
                        value={assignTargetKind}
                        onChange={(e) => {
                            const kind = e.target.value as "user" | "service";
                            setAssignTargetKind(kind);
                            loadAssignTargets(kind);
                        }}
                    >
                        <option value="user">USER</option>
                        <option value="service">SERVICE</option>
                    </select>
                    <select className="input-desktop flex-1" value={assignTargetId} onChange={(e) => setAssignTargetId(e.target.value)}>
                        <option value="">TARGET {assignTargetKind.toUpperCase()} *</option>
                        {assignTargets.map((t) => (
                            <option key={t.id} value={t.id}>
                                {t.label}
                            </option>
                        ))}
                    </select>
                    <select className="input-desktop w-56" value={assignRoleId} onChange={(e) => setAssignRoleId(e.target.value)}>
                        <option value="">ROLE *</option>
                        {roles.map((r) => (
                            <option key={r.id} value={r.id}>
                                #{r.id} {r.name}
                            </option>
                        ))}
                    </select>
                    <button
                        className="btn-desktop-primary"
                        onClick={() => handleAssignRevokeRole(false)}
                        disabled={assigning}
                        title="Grant this role to the target (self-assignment is refused server-side)"
                    >
                        {assigning ? <Loader2 className="animate-spin" size={18} /> : <UserPlus size={18} />}
                        <span>&nbsp;ASSIGN</span>
                    </button>
                    <button
                        className="btn-desktop"
                        onClick={() => handleAssignRevokeRole(true)}
                        disabled={assigning}
                        title="Revoke this role from the target"
                    >
                        <UserMinus size={18} />
                        <span>&nbsp;REVOKE</span>
                    </button>
                </div>

                <div className="query-controls-row" style={{ marginTop: "1.5rem" }}>
                    <input
                        className="input-desktop w-48"
                        placeholder="ROLE NAME"
                        value={newRoleName}
                        onChange={(e) => setNewRoleName(e.target.value)}
                    />
                    <input
                        className="input-desktop flex-1"
                        placeholder="DESCRIPTION"
                        value={newRoleDesc}
                        onChange={(e) => setNewRoleDesc(e.target.value)}
                    />
                    <input
                        className="input-desktop flex-1"
                        placeholder="PERMISSIONS (e.g. storage:read, storage:write)"
                        value={newRolePerms}
                        onChange={(e) => setNewRolePerms(e.target.value)}
                    />
                    <button className="btn-desktop-primary" onClick={handleCreateRole} disabled={creatingRole}>
                        {creatingRole ? <Loader2 className="animate-spin" size={18} /> : <Plus size={18} />}
                        <span>&nbsp;CREATE ROLE</span>
                    </button>
                </div>
            </div>
        </div>
    );
}
