import { useNavigate, useLocation } from "react-router-dom";
import { useAuth } from "@/contexts/AuthContext";
import { APP_NAV_ITEMS } from "@/config/navigation";

export const MobileNav = () => {
    const navigate = useNavigate();
    const location = useLocation();
    const { user } = useAuth();

    const isAdmin = user ? (typeof user.role === "string" ? user.role === "admin" : user.role?.name === "admin") : false;
    const visibleItems = APP_NAV_ITEMS.filter((item) => !item.adminOnly || isAdmin);

    return (
        <nav className="mobile-nav" aria-label="Main navigation">
            {visibleItems.map((item) => {
                const Icon = item.icon;
                const isActive = location.pathname === item.path;

                return (
                    <button
                        key={item.path}
                        type="button"
                        onClick={() => navigate(item.path)}
                        className={`mobile-nav-link ${isActive ? "active" : ""}`}
                        aria-current={isActive ? "page" : undefined}
                    >
                        <Icon size={20} strokeWidth={isActive ? 2.25 : 2} />
                        <span>{item.label}</span>
                    </button>
                );
            })}
        </nav>
    );
};
