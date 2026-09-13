import { useNavigate, useLocation } from "react-router-dom";
import { LayoutDashboard } from "lucide-react";
import { useAuth } from "@/contexts/AuthContext";
import { APP_NAV_ITEMS } from "@/config/navigation";

export const Sidebar = () => {
    const navigate = useNavigate();
    const location = useLocation();
    const { user } = useAuth();

    const isAdmin = user ? (typeof user.role === "string" ? user.role === "admin" : user.role?.name === "admin") : false;
    const visibleItems = APP_NAV_ITEMS.filter((item) => !item.adminOnly || isAdmin);

    return (
        <aside className="desktop-sidebar">
            <div className="sidebar-container">
                <div className="sidebar-logo-area">
                    <LayoutDashboard className="sidebar-logo-icon" size={32} />
                </div>

                <nav className="sidebar-nav" aria-label="Main navigation">
                    <div className="sidebar-section-label">Navigation</div>
                    {visibleItems.map((item) => {
                        const Icon = item.icon;
                        const isActive = location.pathname === item.path;

                        return (
                            <button
                                key={item.path}
                                onClick={() => navigate(item.path)}
                                title={item.label}
                                className={`desktop-sidebar-link group ${isActive ? "active" : ""}`}
                            >
                                <Icon size={20} className="shrink-0" />
                                <span className="sidebar-label">{item.label}</span>
                            </button>
                        );
                    })}
                </nav>
            </div>
        </aside>
    );
};
