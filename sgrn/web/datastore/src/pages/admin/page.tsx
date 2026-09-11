import { useState, useEffect, useCallback } from "react";
import { UserPlus, Users, Settings, Shield, Check, Copy, Database, Play, Terminal, Loader2, Search, RefreshCw } from "lucide-react";
import { PageLayout } from "@/components/PageLayout";
import { fetchOrganisations, fetchDomains, fetchStatuses } from "@/pages/signin/backend";

import { UserStatus } from "@/types";

import type { IdNamePair } from "@/types/IdNamePair";
import { AdminBackendApiEndpoints } from "@/backend/endpoints";
import { useEvent } from "@/contexts/EventContext";
import { authenticatedFetch } from "@/backend/api/fetcher";

// Utility hook for fetching dependencies
function useOrgDependencies(t_selected_org: string | null) {
    const [domains, setDomains] = useState<IdNamePair[]>([]);
    const [statuses, setStatuses] = useState<IdNamePair[]>([]);

    useEffect(() => {
        if (!t_selected_org) {
            setDomains([]);
            setStatuses([]);
            return;
        }
        // Narrowing
        const t_id: string = t_selected_org;
        Promise.all([fetchDomains(t_id).then(setDomains), fetchStatuses(t_id).then(setStatuses)]).catch(() => {
            setDomains([]);
            setStatuses([]);
        });
    }, [t_selected_org]);

    return { domains, statuses };
}

type RegistrationMode = "user" | "automated_service" | "query_builder";

interface AutomatedServiceListEntry {
    id: number;
    name: string;
    token: string;
    metadata: Record<string, unknown>;
    is_active: boolean;
    domain: string;
    created_at: string;
}

interface AdminUserListEntry {
    id: number;
    email: string;
    first_name: string;
    family_name: string;
    domain: string;
    status: UserStatus | string;
}

export default function AdminTab() {
    const { showEvent } = useEvent();
    const [mode, setMode] = useState<RegistrationMode>("user");

    // User State
    const [organisations, setOrganisations] = useState<IdNamePair[]>([]);
    const [selectedOrg, setSelectedOrg] = useState<string | null>(null);
    const { domains, statuses } = useOrgDependencies(selectedOrg);

    // Automated Service State
    const [automatedServiceToken, setAutomatedServiceToken] = useState<string | null>(null);
    const [automatedServiceSecret, setAutomatedServiceSecret] = useState<string | null>(null);
    const [automatedServiceStatusLabel, setAutomatedServiceStatusLabel] = useState<string>("");
    const [automatedServices, setAutomatedServices] = useState<AutomatedServiceListEntry[]>([]);
    const [automatedServiceListError, setAutomatedServiceListError] = useState<string | null>(null);
    const [loadingAutomatedServices, setLoadingAutomatedServices] = useState(false);
    const [rotatingAutomatedServiceId, setRotatingAutomatedServiceId] = useState<number | null>(null);

    // Users State
    const [users, setUsers] = useState<AdminUserListEntry[]>([]);
    const [loadingUsers, setLoadingUsers] = useState(false);

    const [submitting, setSubmitting] = useState(false);

    // Query Builder State
    const [queryTable, setQueryTable] = useState<string>("automated_services");
    const [queryParams, setQueryParams] = useState<string>("");
    const [queryResult, setQueryResult] = useState<any>(null);
    const [queryLoading, setQueryLoading] = useState(false);

    useEffect(() => {
        fetchOrganisations().then(setOrganisations);
    }, []);

    const handleOrgChange = useCallback((t_event: React.ChangeEvent<HTMLSelectElement>) => {
        setSelectedOrg(t_event.target.value || null);
    }, []);

    const fetchAutomatedServices = useCallback(async () => {
        setAutomatedServiceListError(null);
        setLoadingAutomatedServices(true);
        try {
            const response = await authenticatedFetch(AdminBackendApiEndpoints.LIST_AUTOMATED_SERVICES);
            if (!response.ok) {
                throw new Error("Failed to load automated services");
            }
            const payload = await response.json();
            if (!Array.isArray(payload)) {
                throw new Error("Unexpected automated service list format");
            }
            const parsed: AutomatedServiceListEntry[] = payload.map((entry) => ({
                id: Number(entry.id) || 0,
                name: String(entry.name || "Unknown"),
                token: String(entry.token || ""),
                metadata: typeof entry.metadata === "object" && entry.metadata !== null ? entry.metadata : {},
                is_active: Boolean(entry.is_active),
                domain: String(entry.domain || ""),
                created_at: String(entry.created_at || ""),
            }));
            setAutomatedServices(parsed);
        } catch (error) {
            console.error("Failed to fetch automated services:", error);
            showEvent("error", "Unable to load automated service roster");
            setAutomatedServiceListError("Unable to load automated service roster");
        } finally {
            setLoadingAutomatedServices(false);
        }
    }, [showEvent]);

    useEffect(() => {
        if (mode === "automated_service") {
            fetchAutomatedServices();
        }
    }, [fetchAutomatedServices, mode]);

    const fetchUsers = useCallback(async () => {
        setLoadingUsers(true);
        try {
            const response = await authenticatedFetch(AdminBackendApiEndpoints.LIST_USERS);
            if (!response.ok) {
                throw new Error("Failed to load users");
            }
            const payload = await response.json();
            if (!Array.isArray(payload)) {
                throw new Error("Unexpected user list format");
            }
            const parsed: AdminUserListEntry[] = payload.map((entry) => ({
                id: Number(entry.id) || 0,
                email: String(entry.email || ""),
                first_name: String(entry.first_name || ""),
                family_name: String(entry.family_name || ""),
                domain: String(entry.domain || ""),
                status: (entry.status as UserStatus) || UserStatus.ACTIVE,
            }));
            setUsers(parsed);
        } catch (error) {
            console.error("Failed to fetch users:", error);
            showEvent("error", "Unable to load user directory");
        } finally {
            setLoadingUsers(false);
        }
    }, [showEvent]);

    useEffect(() => {
        if (mode === "user") {
            fetchUsers();
        }
    }, [fetchUsers, mode]);

    const rotateAutomatedServiceToken = useCallback(
        async (svcId: number, svcName: string) => {
            setRotatingAutomatedServiceId(svcId);
            try {
                const response = await authenticatedFetch(AdminBackendApiEndpoints.ROTATE_AUTOMATED_SERVICE_TOKEN, {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ automated_service_id: svcId }),
                });
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    throw new Error(payload.error || "Rotation failed");
                }
                setAutomatedServiceToken(payload.token);
                setAutomatedServiceSecret(payload.token_secret);
                setAutomatedServiceStatusLabel(`Credentials rotated for ${svcName}`);
                showEvent("success", `Service ${svcName} tokens rotated`);
                fetchAutomatedServices();
            } catch (error) {
                console.error("Rotation failed:", error);
                showEvent("error", "Unable to rotate automated service token");
            } finally {
                setRotatingAutomatedServiceId(null);
            }
        },
        [fetchAutomatedServices, showEvent],
    );

    const handleRegisterUser = useCallback(
        async (t_event: React.FormEvent<HTMLFormElement>) => {
            t_event.preventDefault();
            const form = new FormData(t_event.currentTarget);
            const formEl = t_event.currentTarget;

            const payload = {
                first_name: form.get("first_name")?.toString().trim() || "",
                family_name: form.get("family_name")?.toString().trim() || "",
                email: form.get("email")?.toString().trim() || "",
                password: form.get("password")?.toString().trim() || "",
                phone_number: form.get("phone")?.toString().trim() || "",
                organisation: form.get("organisation")?.toString() || "",

                status: form.get("status")?.toString() || "",
                domain: form.get("domain")?.toString()?.trim() || "",
            };

            if (
                !payload.first_name ||
                !payload.family_name ||
                !payload.email ||
                !payload.password ||
                !payload.organisation ||
                !payload.status
            ) {
                showEvent("warning", "Please fill in all required fields");
                return;
            }

            setSubmitting(true);
            try {
                const response = await authenticatedFetch(AdminBackendApiEndpoints.REGISTER_USER, {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload),
                });
                const result = await response.json();
                if (response.ok && result.success) {
                    showEvent("success", `User ${payload.email} registered!`);
                    formEl.reset();
                    setSelectedOrg(null);
                    fetchUsers();
                } else {
                    showEvent("error", result.message || "Registration failed");
                }
            } catch {
                showEvent("error", "Server error");
            } finally {
                setSubmitting(false);
            }
        },
        [showEvent, fetchUsers],
    );

    const handleRegisterAutomatedService = useCallback(
        async (t_event: React.FormEvent<HTMLFormElement>) => {
            t_event.preventDefault();
            const form = new FormData(t_event.currentTarget);
            const formEl = t_event.currentTarget;

            const serviceKind = form.get("service_kind")?.toString().trim() || "";
            const payload = {
                name: form.get("service_name")?.toString().trim() || "",
                organisation: form.get("organisation")?.toString() || "",
                metadata: serviceKind ? { kind: serviceKind } : {},
                domain: form.get("domain")?.toString()?.trim() || "",
            };

            if (!payload.name) {
                showEvent("warning", "Please fill in the service name");
                return;
            }

            setSubmitting(true);
            setAutomatedServiceToken(null);
            setAutomatedServiceSecret(null);

            try {
                const response = await authenticatedFetch(AdminBackendApiEndpoints.REGISTER_AUTOMATED_SERVICE, {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify(payload),
                });
                const result = await response.json();
                if (response.ok && result.success) {
                    showEvent("success", `Service ${payload.name} registered!`);
                    setAutomatedServiceToken(result.token);
                    setAutomatedServiceSecret(result.token_secret);
                    setAutomatedServiceStatusLabel(`Service ${payload.name} credentials ready`);
                    fetchAutomatedServices();
                    formEl.reset();
                } else {
                    showEvent("error", result.message || result.error || "Registration failed");
                }
            } catch {
                showEvent("error", "Server error");
            } finally {
                setSubmitting(false);
            }
        },
        [showEvent, fetchAutomatedServices],
    );

    const executeQuery = async () => {
        setQueryLoading(true);
        setQueryResult(null);
        try {
            const endpoint = `/api/v1/postgrest/${queryTable}`;
            const url = queryParams ? `${endpoint}?${queryParams.startsWith("?") ? queryParams.slice(1) : queryParams}` : endpoint;

            const response = await authenticatedFetch(url);
            const data = await response.json();
            setQueryResult(data);
            if (!response.ok) {
                showEvent("error", `Query failed: ${data.message || response.statusText}`);
            } else {
                showEvent("success", "Query executed successfully");
            }
        } catch (error) {
            console.error("Query failed:", error);
            showEvent("error", "Failed to execute query.");
        } finally {
            setQueryLoading(false);
        }
    };

    const copyToClipboard = (text: string) => {
        navigator.clipboard.writeText(text);
        showEvent("success", "Copied to clipboard");
    };

    const renderOptions = (t_items: IdNamePair[]) =>
        t_items.map((t_item) => (
            <option key={t_item.id} value={t_item.name}>
                {t_item.name}
            </option>
        ));

    return (
        <PageLayout>
            <div className="reg-layout admin-shell">
                <div className="reg-card">
                    {/* Mode Switcher */}
                    <div className="admin-mode-switch">
                        <button
                            className={`admin-mode-btn ${mode === "user" ? "admin-mode-btn-active" : ""}`}
                            onClick={() => setMode("user")}
                        >
                            <Users size={16} /> <span>User Management</span>
                        </button>
                        <button
                            className={`admin-mode-btn ${mode === "automated_service" ? "admin-mode-btn-active" : ""}`}
                            onClick={() => setMode("automated_service")}
                        >
                            <Settings size={16} /> <span>Service Provisioning</span>
                        </button>
                        <button
                            className={`admin-mode-btn ${mode === "query_builder" ? "admin-mode-btn-active" : ""}`}
                            onClick={() => setMode("query_builder")}
                        >
                            <Database size={16} /> <span>Industrial Data Plane</span>
                        </button>
                    </div>

                    <div className="reg-header">
                        {mode === "user" && <UserPlus size={20} className="text-primary" />}
                        {mode === "automated_service" && <Shield size={20} className="text-primary" />}
                        {mode === "query_builder" && <Search size={20} className="text-primary" />}
                        <h2 className="reg-title">
                            {mode === "user" && "Register User"}
                            {mode === "automated_service" && "Register Service"}
                            {mode === "query_builder" && "PostgREST Explorer"}
                        </h2>
                    </div>

                    {mode === "user" && (
                        <>
                            <form onSubmit={handleRegisterUser} className="reg-form">
                                <div className="reg-input-group">
                                    <input name="first_name" placeholder="FIRST NAME *" className="input-desktop" required maxLength={64} />
                                    <input
                                        name="family_name"
                                        placeholder="FAMILY NAME *"
                                        className="input-desktop"
                                        required
                                        maxLength={64}
                                    />
                                </div>
                                <div className="reg-input-group">
                                    <input
                                        name="email"
                                        type="email"
                                        placeholder="EMAIL ADDRESS *"
                                        className="input-desktop"
                                        required
                                        maxLength={128}
                                    />
                                    <input name="phone" type="tel" placeholder="TERMINAL NUMBER" className="input-desktop" />
                                </div>
                                <div className="reg-input-group">
                                    <input
                                        name="password"
                                        type="password"
                                        placeholder="INITIAL ACCESS KEY *"
                                        className="input-desktop"
                                        required
                                    />
                                    <select
                                        name="organisation"
                                        className="input-desktop"
                                        defaultValue=""
                                        onChange={handleOrgChange}
                                        required
                                    >
                                        <option value="" disabled>
                                            SELECT ORGANISATION *
                                        </option>
                                        {renderOptions(organisations)}
                                    </select>
                                </div>
                                <div className="reg-input-group-3">
                                    <select name="status" className="input-desktop" defaultValue="" required disabled={!selectedOrg}>
                                        <option value="" disabled>
                                            USER STATUS *
                                        </option>
                                        {renderOptions(statuses)}
                                    </select>
                                    <select name="domain" className="input-desktop" defaultValue="" required disabled={!selectedOrg}>
                                        <option value="" disabled>
                                            NETWORK DOMAIN *
                                        </option>
                                        {renderOptions(domains)}
                                    </select>
                                    <button type="submit" className="btn-desktop-primary" disabled={submitting || !selectedOrg}>
                                        {submitting ? "PROCESSING..." : "REGISTER USER"}
                                    </button>
                                </div>
                            </form>

                            <div className="roster-container">
                                <div className="roster-toolbar">
                                    <h3 className="roster-title">User Directory</h3>
                                    <button onClick={fetchUsers} className="btn-desktop">
                                        <RefreshCw size={14} className={loadingUsers ? "drive-spin" : ""} />
                                    </button>
                                </div>
                                <div className="panel">
                                    <table className="datagrid-industrial">
                                        <thead>
                                            <tr>
                                                <th>ID</th>
                                                <th>NAME</th>
                                                <th>EMAIL</th>
                                                <th>DOMAIN</th>
                                                <th>STATUS</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {users.map((u) => (
                                                <tr key={u.id}>
                                                    <td className="admin-cell-muted">#{u.id}</td>
                                                    <td className="admin-cell-bold">
                                                        {u.first_name} {u.family_name}
                                                    </td>
                                                    <td>{u.email}</td>
                                                    <td className="admin-cell-primary">{u.domain || "GLOBAL"}</td>
                                                    <td>
                                                        <span className="badge-status badge-status--active">ACTIVE</span>
                                                    </td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                            </div>
                        </>
                    )}

                    {mode === "automated_service" && (
                        <>
                            <form onSubmit={handleRegisterAutomatedService} className="reg-form">
                                <div className="reg-input-group">
                                    <input name="service_name" placeholder="SERVICE IDENTIFIER *" className="input-desktop" required />
                                    <select
                                        name="organisation"
                                        className="input-desktop"
                                        defaultValue=""
                                        onChange={handleOrgChange}
                                        required
                                    >
                                        <option value="" disabled>
                                            SELECT ORGANISATION *
                                        </option>
                                        {renderOptions(organisations)}
                                    </select>
                                </div>
                                <div className="reg-input-group-3">
                                    <select name="service_kind" className="input-desktop" defaultValue="">
                                        <option value="" disabled>
                                            SERVICE CLASSIFICATION
                                        </option>
                                        <option value="plc">PLC (S7/OPC-UA)</option>
                                        <option value="service">C++ INDUSTRIAL MODULE</option>
                                        <option value="integration">EXTERNAL CLOUD HUB</option>
                                    </select>
                                    <select name="domain" className="input-desktop" defaultValue="" required disabled={!selectedOrg}>
                                        <option value="" disabled>
                                            NETWORK DOMAIN *
                                        </option>
                                        {renderOptions(domains)}
                                    </select>
                                    <button type="submit" className="btn-desktop-primary" disabled={submitting}>
                                        {submitting ? "INITIALIZING..." : "PROVISION SERVICE"}
                                    </button>
                                </div>
                            </form>

                            {automatedServiceToken && (
                                <div className="credentials-card">
                                    <div className="credentials-header">
                                        <Check size={16} /> {automatedServiceStatusLabel}
                                    </div>
                                    <div className="credential-field">
                                        <label>TOKEN</label>
                                        <div className="credential-value">
                                            <code>{automatedServiceToken}</code>
                                            <button onClick={() => copyToClipboard(automatedServiceToken)}>
                                                <Copy size={14} />
                                            </button>
                                        </div>
                                    </div>
                                    {automatedServiceSecret && (
                                        <div className="credential-field">
                                            <label>SECRET</label>
                                            <div className="credential-value">
                                                <code>{automatedServiceSecret}</code>
                                                <button onClick={() => copyToClipboard(automatedServiceSecret)}>
                                                    <Copy size={14} />
                                                </button>
                                            </div>
                                        </div>
                                    )}
                                </div>
                            )}

                            <div className="roster-container">
                                <div className="roster-toolbar">
                                    <h3 className="roster-title">Service Inventory</h3>
                                    <button onClick={fetchAutomatedServices} className="btn-desktop">
                                        <RefreshCw size={14} className={loadingAutomatedServices ? "drive-spin" : ""} />
                                    </button>
                                </div>
                                {automatedServiceListError && <div className="admin-service-error">{automatedServiceListError}</div>}
                                <div className="panel">
                                    <table className="datagrid-industrial">
                                        <thead>
                                            <tr>
                                                <th>IDENTIFIER</th>
                                                <th>DOMAIN</th>
                                                <th>TOKEN REFERENCE</th>
                                                <th className="text-right">OPERATIONS</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {automatedServices.map((svc) => (
                                                <tr key={svc.id}>
                                                    <td className="admin-cell-bold">{svc.name}</td>
                                                    <td className="admin-cell-primary">{svc.domain || "GLOBAL"}</td>
                                                    <td className="admin-cell-token">{svc.token}</td>
                                                    <td>
                                                        <div className="admin-actions-right">
                                                            <button
                                                                className="btn-desktop h-7 px-3"
                                                                onClick={() => rotateAutomatedServiceToken(svc.id, svc.name)}
                                                                disabled={rotatingAutomatedServiceId === svc.id}
                                                            >
                                                                {rotatingAutomatedServiceId === svc.id ? "ROTATING..." : "ROTATE KEY"}
                                                            </button>
                                                        </div>
                                                    </td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                            </div>
                        </>
                    )}

                    {mode === "query_builder" && (
                        <div className="query-builder">
                            <div className="query-controls">
                                <div className="query-controls-row">
                                    <select
                                        className="input-desktop w-64"
                                        value={queryTable}
                                        onChange={(e) => setQueryTable(e.target.value)}
                                    >
                                        <option value="automated_services">AUTOMATED SERVICES</option>
                                        <option value="users">USER ROSTER</option>
                                        <option value="organisations">ORGANISATIONS</option>
                                        <option value="domains">DOMAINS</option>
                                        <option value="files">STORAGE FILES</option>
                                    </select>
                                    <input
                                        className="input-desktop flex-1"
                                        placeholder="DATA FILTER (e.g. select=*,id=eq.1)"
                                        value={queryParams}
                                        onChange={(e) => setQueryParams(e.target.value)}
                                    />
                                    <button className="btn-desktop-primary w-12" onClick={executeQuery} disabled={queryLoading}>
                                        {queryLoading ? <Loader2 className="animate-spin" size={18} /> : <Play size={18} />}
                                    </button>
                                </div>
                            </div>

                            <div className="json-explorer">
                                <div className="explorer-header">
                                    <div className="explorer-header-left">
                                        <Terminal size={14} className="admin-cell-primary" />
                                        <span className="explorer-label">DATA PLANE SNAPSHOT</span>
                                    </div>
                                    {queryResult && (
                                        <button
                                            className="btn-desktop h-7 px-3"
                                            onClick={() => copyToClipboard(JSON.stringify(queryResult, null, 2))}
                                        >
                                            EXPORT JSON
                                        </button>
                                    )}
                                </div>
                                <div className="explorer-body">
                                    {queryResult ? (
                                        <pre>{JSON.stringify(queryResult, null, 2)}</pre>
                                    ) : (
                                        <div className="empty-state">Execute a query to inspect industrial data planes.</div>
                                    )}
                                </div>
                            </div>

                            <div className="query-help">
                                <strong>Syntax:</strong> column=eq.val | select=id,name | order=created_at.desc | limit=10
                            </div>
                        </div>
                    )}

                    <p className="reg-footer">* Required fields</p>
                </div>
            </div>
        </PageLayout>
    );
}
