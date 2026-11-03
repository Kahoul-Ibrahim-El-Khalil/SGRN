// @/components/TopBar.tsx
// Combined desktop top bar: horizontally merges the navigation links
// (previously the sidebar) with the header actions (previously the top bar).
// It is collapsed by default (a thin strip at the top edge) and slides open
// on hover — see .desktop-topbar-zone in layers/shell.css.
import { useNavigate, useLocation } from "react-router-dom";
import { LayoutDashboard, LogOut, Bell } from "lucide-react";
import { useAuth } from "@/contexts/AuthContext";
import { APP_NAV_ITEMS } from "@/config/navigation";
import { handleSignOut } from "@/pages/signin/backend";
import ThemeToggle from "@/components/ThemeToggle";

export const TopBar = () => {
    const navigate = useNavigate();
    const location = useLocation();
    const { user } = useAuth();

    const isAdmin = user ? (typeof user.role === "string" ? user.role === "admin" : user.role?.name === "admin") : false;
    const visibleItems = APP_NAV_ITEMS.filter((item) => !item.adminOnly || isAdmin);

    const handleSignOutClick = async () => {
        await handleSignOut();
        navigate("/signin");
    };

    return (
        <header className="desktop-topbar-zone">
            <div className="desktop-topbar">
                <nav className="topbar-nav" aria-label="Main navigation">
                    <div className="topbar-logo-area">
                        <LayoutDashboard className="sidebar-logo-icon" size={16} />
                    </div>
                    {visibleItems.map((item) => {
                        const Icon = item.icon;
                        const isActive = location.pathname === item.path;

                        return (
                            <button
                                key={item.path}
                                type="button"
                                onClick={() => navigate(item.path)}
                                title={item.label}
                                className={`topbar-link ${isActive ? "active" : ""}`}
                                aria-current={isActive ? "page" : undefined}
                            >
                                <Icon size={20} className="shrink-0" />
                                <span className="topbar-label">{item.label}</span>
                            </button>
                        );
                    })}
                </nav>

                <div className="header-actions">
                    <div className="header-utility-group">
                        <button type="button" className="header-icon-btn" aria-label="Notifications">
                            <Bell size={15} />
                        </button>
                        <ThemeToggle />
                    </div>
                    <div className="header-toolbar-sep" aria-hidden="true" />
                    <div className="header-user-area">
                        <div className="header-user-info">
                            <span className="header-user-name">
                                {user?.first_name} {user?.family_name}
                            </span>
                            <span className="header-user-role">{isAdmin ? "Admin Access" : "Standard User"}</span>
                        </div>
                        <button onClick={handleSignOutClick} className="header-signout-btn" title="Sign Out">
                            <LogOut size={18} />
                        </button>
                    </div>
                </div>
            </div>
        </header>
    );
};
