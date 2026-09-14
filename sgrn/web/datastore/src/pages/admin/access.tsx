import { useState, useCallback } from "react";
import { Loader2, Search, Plus, Trash2 } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import { fetchPermissions, grantPermission, revokePermission, type DomainPermission } from "@/backend/api/permissions";

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
                    <div className="roster-title">
                        DOMAIN GRANTS — {rows.length} ROW{rows.length === 1 ? "" : "S"}
                    </div>
                    <div className="datagrid-wrapper">
                        <table className="datagrid-industrial">
                            <thead>
                                <tr>
                                    <th>USER</th>
                                    <th>DOMAIN</th>
                                    <th>SUBPATH</th>
                                    <th>R / W / D</th>
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
                                        <td>{r.allowed_subpath}</td>
                                        <td>{[r.can_read ? "R" : "–", r.can_write ? "W" : "–", r.can_delete ? "D" : "–"].join(" ")}</td>
                                        <td>
                                            <div className="admin-actions-right">
                                                <button
                                                    className={`btn-desktop h-7 px-3 ${armingDelete === r.id ? "btn-desktop-primary" : ""}`}
                                                    onClick={() => handleRevoke(r)}
                                                >
                                                    {armingDelete === r.id ? "CONFIRM" : <Trash2 size={14} />}
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
                <div className="roster-title">GRANT DOMAIN ACCESS</div>
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
                unlocks them, revoking the last one locks them out again (the API warns in both cases). Subpath sandboxes them inside one
                tree; the domain must already exist in the user's organisation.
            </div>
        </div>
    );
}
