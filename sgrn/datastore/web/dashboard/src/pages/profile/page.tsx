// @/pages/profile/page.tsx
//
// Architecture overview:
//   Custom Hooks (logic)
//     useStorageStats   – fetches & holds storage telemetry
//     useProfileForm    – profile field state + submit handler
//     usePasswordForm   – password field state + submit handler
//
//   Local Components (UI)
//     UserInfoCard      – avatar, name, role badge, email, org
//     StorageStatRow    – labelled progress-bar row (reusable inside stats)
//     StorageStatsCard  – storage telemetry panel
//     ProfileForm       – profile fields form card
//     PasswordForm      – password change form card
//     SecurityAlert     – bottom protocol-alert banner

import { useState, useEffect } from "react";
import { motion } from "framer-motion";
import { useNavigate } from "react-router-dom";
import { Lock, Loader2, User, Mail, Shield, Building2, Phone, HardDrive, Zap } from "lucide-react";

import { PageLayout } from "@/components/PageLayout";
import { useAuth } from "@/contexts/AuthContext";
import { useEvent } from "@/contexts/EventContext";
import { authenticatedFetch, processResponse } from "@/backend/api/fetcher";
import { SessionBackendApiEndpoints, QueryListBackendApiEndpoints, StorageBackendApiEndpoints } from "@/backend/endpoints";
import { handleSgrnResult } from "@/backend/errorHandler";

// ─── types ───────────────────────────────────────────────────────────────────

interface StorageStats {
    file_count: number;
    total_original_bytes: number;
    total_compressed_bytes: number;
    storage_limit: number | null;
}

// ─── helpers ─────────────────────────────────────────────────────────────────

function formatBytes(bytes: number): string {
    if (bytes === 0) return "0 Bytes";
    const k = 1024;
    const sizes = ["Bytes", "KB", "MB", "GB", "TB"];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return `${parseFloat((bytes / Math.pow(k, i)).toFixed(2))} ${sizes[i]}`;
}

// ─── custom hooks ────────────────────────────────────────────────────────────

/** Fetches storage telemetry for the current user. */
function useStorageStats(userId: number | string | undefined) {
    const [stats, setStats] = useState<StorageStats | null>(null);
    const [loading, setLoading] = useState(true);

    useEffect(() => {
        if (!userId) return;

        let cancelled = false;
        (async () => {
            try {
                const res = await authenticatedFetch(StorageBackendApiEndpoints.GET_STATS);
                const result = await processResponse<StorageStats>(res);
                if (!cancelled && handleSgrnResult(result)) setStats(result.data);
            } catch (err) {
                console.error("Failed to fetch storage stats", err);
            } finally {
                if (!cancelled) setLoading(false);
            }
        })();

        return () => {
            cancelled = true;
        };
    }, [userId]);

    return { stats, loading };
}

/** Manages the profile info form state and submission. */
function useProfileForm(
    user: ReturnType<typeof useAuth>["user"],
    login: ReturnType<typeof useAuth>["login"],
    showEvent: ReturnType<typeof useEvent>["showEvent"],
) {
    const [loading, setLoading] = useState(false);
    const [fields, setFields] = useState({
        first_name: user?.first_name ?? "",
        family_name: user?.family_name ?? "",
        phone_number: user?.phone_number ?? "",
    });

    const handleChange = (e: React.ChangeEvent<HTMLInputElement>) => setFields((prev) => ({ ...prev, [e.target.name]: e.target.value }));

    const handleSubmit = async (e: React.FormEvent) => {
        e.preventDefault();
        setLoading(true);
        try {
            const res = await authenticatedFetch(QueryListBackendApiEndpoints.UPDATE_USER_INFO, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(fields),
            });
            const result = await processResponse<{ message: string }>(res);
            if (handleSgrnResult(result)) {
                showEvent("success", "Profile updated successfully!");
                if (user) {
                    const token = sessionStorage.getItem("SGRN-TOKEN") || localStorage.getItem("SGRN-TOKEN") || "";
                    login({ ...user, ...fields }, token);
                }
            }
        } catch {
            showEvent("error", "Failed to update profile info.");
        } finally {
            setLoading(false);
        }
    };

    return { fields, loading, handleChange, handleSubmit };
}

/** Manages the password change form state and submission. */
function usePasswordForm(
    logout: ReturnType<typeof useAuth>["logout"],
    showEvent: ReturnType<typeof useEvent>["showEvent"],
    navigate: ReturnType<typeof useNavigate>,
) {
    const [loading, setLoading] = useState(false);
    const [fields, setFields] = useState({
        old_password: "",
        new_password: "",
        confirm_password: "",
    });

    const handleChange = (e: React.ChangeEvent<HTMLInputElement>) => setFields((prev) => ({ ...prev, [e.target.name]: e.target.value }));

    const handleSubmit = async (e: React.FormEvent) => {
        e.preventDefault();

        if (fields.new_password !== fields.confirm_password) return showEvent("error", "New passwords do not match");
        if (fields.new_password.length < 8) return showEvent("warning", "Password must be at least 8 characters long");

        setLoading(true);
        try {
            const res = await authenticatedFetch(SessionBackendApiEndpoints.UPDATE_PASSWORD, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                    old_password: fields.old_password,
                    new_password: fields.new_password,
                }),
            });
            const result = await processResponse<{ message: string }>(res);
            if (handleSgrnResult(result)) {
                showEvent("success", result.data.message || "Password updated successfully!");
                setTimeout(() => {
                    logout();
                    navigate("/signin");
                }, 2000);
            }
        } catch {
            showEvent("error", "Failed to update password. Please try again.");
        } finally {
            setLoading(false);
        }
    };

    return { fields, loading, handleChange, handleSubmit };
}

// ─── local components ────────────────────────────────────────────────────────

interface UserInfoCardProps {
    user: NonNullable<ReturnType<typeof useAuth>["user"]>;
    isAdmin: boolean;
}

function UserInfoCard({ user, isAdmin }: UserInfoCardProps) {
    const orgName = typeof user.organisation === "string" ? user.organisation : (user.organisation as any)?.name || "N/A";

    return (
        <div className="profile-card profile-card-widget">
            <div className="profile-avatar">
                <User size={48} />
            </div>
            <h2 className="profile-name">
                {user.first_name} {user.family_name}
            </h2>
            <div className={`profile-role-badge ${isAdmin ? "profile-role-badge--admin" : ""}`}>
                {isAdmin && <Shield size={12} />}
                <span>{isAdmin ? "Administrator" : "Standard User"}</span>
            </div>

            <div className="profile-details">
                <div className="profile-detail-group">
                    <label className="profile-detail-label">Email Address</label>
                    <div className="profile-detail-value">
                        <Mail size={12} className="profile-detail-icon" />
                        {user.email}
                    </div>
                </div>
                <div className="profile-detail-group">
                    <label className="profile-detail-label">Organisation</label>
                    <div className="profile-detail-value">
                        <Building2 size={12} className="profile-detail-icon" />
                        {orgName}
                    </div>
                </div>
            </div>
        </div>
    );
}

interface StorageStatRowProps {
    label: string;
    value: string;
    usedBytes: number;
    limitBytes: number | null | undefined;
    subLeft: string;
    subRight: string;
    warning?: boolean;
}

function StorageStatRow({ label, value, usedBytes, limitBytes, subLeft, subRight, warning }: StorageStatRowProps) {
    const pct = limitBytes ? Math.min(100, (usedBytes / limitBytes) * 100) : 0;

    return (
        <div className="stats-row">
            <div className="stats-row-top">
                <span className="stats-label">{label}</span>
                <span className="stats-value">{value}</span>
            </div>
            <div className="storage-progress-container">
                <div className={`storage-progress-bar ${warning ? "storage-progress-bar-warning" : ""}`} style={{ width: `${pct}%` }} />
            </div>
            <div className="stats-sub">
                <span>{subLeft}</span>
                <span>{subRight}</span>
            </div>
        </div>
    );
}

interface StorageStatsCardProps {
    stats: StorageStats | null;
    loading: boolean;
    user: NonNullable<ReturnType<typeof useAuth>["user"]>;
}

function StorageStatsCard({ stats, loading, user }: StorageStatsCardProps) {
    const compressionSavings =
        stats && stats.total_original_bytes > 0 ? Math.round((1 - stats.total_compressed_bytes / stats.total_original_bytes) * 100) : 0;

    const storageWarning = !!stats?.storage_limit && stats.total_compressed_bytes / stats.storage_limit > 0.9;

    return (
        <div className="stats-card profile-stats-widget">
            <div className="stats-header">
                <HardDrive size={16} className="text-primary" />
                <span className="stats-title">Storage Status</span>
            </div>
            <div className="stats-body">
                {loading ? (
                    <div style={{ display: "flex", justifyContent: "center", padding: "1rem" }}>
                        <Loader2 size={24} className="drive-spin text-muted-foreground" />
                    </div>
                ) : stats ? (
                    <>
                        <StorageStatRow
                            label="Data Volume"
                            value={formatBytes(stats.total_compressed_bytes)}
                            usedBytes={stats.total_compressed_bytes}
                            limitBytes={stats.storage_limit}
                            subLeft={`RAW: ${formatBytes(stats.total_original_bytes)}`}
                            subRight={`QUOTA: ${stats.storage_limit ? formatBytes(stats.storage_limit) : "INF"}`}
                            warning={storageWarning}
                        />

                        {compressionSavings > 0 && (
                            <div className="stats-banner">
                                <Zap size={14} className="text-primary" />
                                <span className="stats-banner-text">-{compressionSavings}% STORAGE REDUCTION (ZSTD)</span>
                            </div>
                        )}

                        <div className="stats-divider">
                            <StorageStatRow
                                label="Object Registry"
                                value={String(user.total_entry_count ?? stats.file_count)}
                                usedBytes={user.total_entry_count ?? stats.file_count}
                                limitBytes={user.entry_count_limit}
                                subLeft={`REG: ${user.total_entry_count ?? stats.file_count}`}
                                subRight={`LIMIT: ${user.entry_count_limit ?? "INF"}`}
                            />
                        </div>
                    </>
                ) : (
                    <div style={{ fontSize: "11px", color: "hsl(var(--muted-foreground))", fontStyle: "italic" }}>
                        TELEMETRY UNAVAILABLE
                    </div>
                )}
            </div>
        </div>
    );
}

interface ProfileFormProps {
    fields: ReturnType<typeof useProfileForm>["fields"];
    loading: boolean;
    onChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
    onSubmit: (e: React.FormEvent) => void;
}

function ProfileForm({ fields, loading, onChange, onSubmit }: ProfileFormProps) {
    return (
        <div className="reg-card profile-config-widget">
            <div className="reg-header">
                <User size={18} className="text-primary" />
                <h3 className="reg-title">Profile Configuration</h3>
            </div>
            <form onSubmit={onSubmit} className="reg-form">
                <div className="reg-input-group">
                    <div className="credential-field">
                        <label>First Name</label>
                        <input
                            name="first_name"
                            value={fields.first_name}
                            onChange={onChange}
                            className="input-desktop"
                            placeholder="GIVEN NAME"
                            disabled={loading}
                            required
                        />
                    </div>
                    <div className="credential-field">
                        <label>Family Name</label>
                        <input
                            name="family_name"
                            value={fields.family_name}
                            onChange={onChange}
                            className="input-desktop"
                            placeholder="SURNAME"
                            disabled={loading}
                            required
                        />
                    </div>
                </div>

                <div className="credential-field" style={{ maxWidth: "300px" }}>
                    <label>Terminal Number</label>
                    <div style={{ position: "relative" }}>
                        <Phone
                            size={14}
                            style={{
                                position: "absolute",
                                left: "0.75rem",
                                top: "50%",
                                transform: "translateY(-50%)",
                                color: "hsl(var(--muted-foreground))",
                            }}
                        />
                        <input
                            name="phone_number"
                            className="input-desktop"
                            style={{ paddingLeft: "2.5rem" }}
                            value={fields.phone_number}
                            onChange={onChange}
                            placeholder="+CC 000 000 000"
                            disabled={loading}
                        />
                    </div>
                </div>

                <div className="admin-actions-right" style={{ marginTop: "1rem" }}>
                    <button type="submit" className="btn-desktop-primary" style={{ padding: "0.5rem 2rem" }} disabled={loading}>
                        {loading ? "PROCESSING..." : "COMMIT CHANGES"}
                    </button>
                </div>
            </form>
        </div>
    );
}

interface PasswordFormProps {
    fields: ReturnType<typeof usePasswordForm>["fields"];
    loading: boolean;
    onChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
    onSubmit: (e: React.FormEvent) => void;
}

function PasswordForm({ fields, loading, onChange, onSubmit }: PasswordFormProps) {
    return (
        <div className="reg-card profile-security-widget">
            <div className="reg-header">
                <Lock size={18} className="text-primary" />
                <h3 className="reg-title">Security Credentials</h3>
            </div>
            <form onSubmit={onSubmit} className="reg-form">
                <div className="credential-field" style={{ maxWidth: "300px" }}>
                    <label>Current Password</label>
                    <input
                        name="old_password"
                        type="password"
                        autoComplete="current-password"
                        value={fields.old_password}
                        onChange={onChange}
                        className="input-desktop"
                        placeholder="••••••••"
                        disabled={loading}
                        required
                    />
                </div>

                <div className="reg-input-group">
                    <div className="credential-field">
                        <label>New Access Key</label>
                        <input
                            name="new_password"
                            type="password"
                            autoComplete="new-password"
                            value={fields.new_password}
                            onChange={onChange}
                            className="input-desktop"
                            placeholder="••••••••"
                            disabled={loading}
                            required
                        />
                    </div>
                    <div className="credential-field">
                        <label>Confirm Access Key</label>
                        <input
                            name="confirm_password"
                            type="password"
                            autoComplete="new-password"
                            value={fields.confirm_password}
                            onChange={onChange}
                            className="input-desktop"
                            placeholder="••••••••"
                            disabled={loading}
                            required
                        />
                    </div>
                </div>

                <div className="admin-actions-right" style={{ marginTop: "1rem" }}>
                    <button type="submit" className="btn-desktop-secondary" style={{ padding: "0.5rem 2rem" }} disabled={loading}>
                        {loading ? "UPDATING..." : "ROTATE PASSWORD"}
                    </button>
                </div>
            </form>
        </div>
    );
}

function SecurityAlert() {
    return (
        <div className="security-alert profile-alert-widget">
            <Shield size={18} className="text-primary shrink-0" />
            <p>
                <strong>PROTOCOL ALERT:</strong> Modifying your access credentials will terminate all active session tokens across the
                industrial network. Re-authentication will be mandatory on all terminals.
            </p>
        </div>
    );
}

// ─── page ────────────────────────────────────────────────────────────────────

export default function ProfilePage() {
    const { user, login, logout } = useAuth();
    const { showEvent } = useEvent();
    const navigate = useNavigate();

    const isAdmin = typeof user?.role === "string" ? user.role === "admin" : user?.role?.name === "admin";

    const { stats, loading: statsLoading } = useStorageStats(user?.id);
    const profile = useProfileForm(user, login, showEvent);
    const password = usePasswordForm(logout, showEvent, navigate);

    if (!user) return null;

    return (
        <PageLayout>
            <motion.div
                initial={{ opacity: 0, y: 10 }}
                animate={{ opacity: 1, y: 0 }}
                transition={{ duration: 0.3 }}
                className="profile-layout"
            >
                <UserInfoCard user={user} isAdmin={isAdmin} />
                <StorageStatsCard stats={stats} loading={statsLoading} user={user} />
                <ProfileForm
                    fields={profile.fields}
                    loading={profile.loading}
                    onChange={profile.handleChange}
                    onSubmit={profile.handleSubmit}
                />
                <PasswordForm
                    fields={password.fields}
                    loading={password.loading}
                    onChange={password.handleChange}
                    onSubmit={password.handleSubmit}
                />
                <SecurityAlert />
            </motion.div>
        </PageLayout>
    );
}
