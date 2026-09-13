import { useNavigate } from "react-router-dom";
import { LogOut, Bell } from "lucide-react";
import { useAuth } from "@/contexts/AuthContext";
import { handleSignOut } from "@/pages/signin/backend";
import ThemeToggle from "@/components/ThemeToggle";

export const NavigationBar = () => {
    const navigate = useNavigate();
    const { user } = useAuth();
    const isAdmin = user ? (typeof user.role === "string" ? user.role === "admin" : user.role?.name === "admin") : false;

    const handleSignOutClick = async () => {
        await handleSignOut();
        navigate("/signin");
    };

    return (
        <header className="desktop-header">
            <div className="header-logo-container">
                <div className="header-logo-orb" aria-hidden="true" />
                <span className="header-logo-text">
                    SGRN <span className="header-logo-subtitle">Resource Platform</span>
                </span>
            </div>

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
        </header>
    );
};
