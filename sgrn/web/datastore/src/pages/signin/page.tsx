// @/pages/signin/page.tsx
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { useAuth } from "@/contexts/AuthContext";
import { useEvent } from "@/contexts/EventContext";
import { doSignIn } from "@/pages/signin/backend";
import { useAuthForm } from "@/pages/signin/hooks/useAuthForm";
import { useClock } from "@/pages/signin/hooks/useClock";
import { handleSgrnResult } from "@/backend/errorHandler";
import { Logo } from "@/components/Logo";
import ThemeToggle from "@/components/ThemeToggle";
import { Shield, Terminal } from "lucide-react";

function useSignIn(
    formData: ReturnType<typeof useAuthForm>["formData"],
    login: ReturnType<typeof useAuth>["login"],
    showEvent: ReturnType<typeof useEvent>["showEvent"],
    navigate: ReturnType<typeof useNavigate>,
) {
    const [loading, setLoading] = useState(false);

    const handleSubmit = async (e: React.FormEvent) => {
        e.preventDefault();
        if (!formData.email || !formData.password) return showEvent("warning", "Please fill all fields");

        setLoading(true);
        try {
            const result = await doSignIn(formData);
            if (!handleSgrnResult(result)) return;
            const { user, token } = result.data;
            login(user, token);
            showEvent("success", "Sign in successful!");
            const isAdmin = typeof user.role === "string" ? user.role === "admin" : user.role?.name === "admin";
            navigate(isAdmin ? "/admin" : "/drive", { replace: true });
        } catch {
            showEvent("error", "Network error during sign in");
        } finally {
            setLoading(false);
        }
    };

    return { loading, handleSubmit };
}

export default function SignInPage() {
    const navigate = useNavigate();
    const { showEvent } = useEvent();
    const { login } = useAuth();
    const { formData, handleChange } = useAuthForm();
    const { timeStr, dateStr } = useClock();
    const { loading, handleSubmit } = useSignIn(formData, login, showEvent, navigate);

    return (
        <div className="signin-root">
            {/* Top logos bar */}
            <header className="signin-topbar">
                <Logo size="sm" showDivider={true} />
                <ThemeToggle />
            </header>

            {/* Split body */}
            <main className="signin-body">
                {/* Left panel — branding */}
                <div className="signin-left">
                    <div className="signin-brand">
                        <div className="signin-brand-icon">
                            <Shield size={40} />
                        </div>
                        <h1 className="signin-brand-title">SGRN</h1>
                        <p className="signin-brand-sub">Industrial Gateway Platform</p>
                        <p className="signin-brand-desc">
                            Secure, real-time access to industrial data streams, telemetry objects and storage namespaces.
                        </p>
                    </div>

                    <div className="signin-clock">
                        <div className="signin-clock-time">{timeStr}</div>
                        <div className="signin-clock-date">{dateStr}</div>
                        <div className="signin-clock-indicator">
                            <Terminal size={10} />
                            <span>SYSTEM ONLINE</span>
                        </div>
                    </div>
                </div>

                {/* Right panel — form */}
                <div className="signin-right">
                    <div className="signin-form-card">
                        <div className="signin-form-header">
                            <h2 className="signin-form-title">AUTHENTICATE</h2>
                            <p className="signin-form-subtitle">Enter your credentials to access the platform</p>
                        </div>

                        <form onSubmit={handleSubmit} className="signin-form">
                            <div className="signin-field">
                                <label className="signin-label">EMAIL ADDRESS</label>
                                <input
                                    className="input-desktop"
                                    type="email"
                                    name="email"
                                    value={formData.email}
                                    onChange={handleChange}
                                    placeholder="operator@domain.com"
                                    disabled={loading}
                                    required
                                />
                            </div>
                            <div className="signin-field">
                                <label className="signin-label">ACCESS KEY</label>
                                <input
                                    className="input-desktop"
                                    type="password"
                                    name="password"
                                    value={formData.password}
                                    onChange={handleChange}
                                    placeholder="••••••••••••"
                                    disabled={loading}
                                    required
                                />
                            </div>
                            <button type="submit" className="btn-desktop-primary signin-submit" disabled={loading}>
                                {loading ? "INITIALIZING SESSION..." : "AUTHENTICATE →"}
                            </button>
                        </form>
                    </div>
                </div>
            </main>
        </div>
    );
}
