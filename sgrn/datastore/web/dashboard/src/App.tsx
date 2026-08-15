// @/App.tsx
import { Suspense, lazy } from "react";
import { Routes, Route, Navigate } from "react-router-dom";
import { Loader2 } from "lucide-react";

import { AuthProvider, useAuth } from "@/contexts/AuthContext";
import { EventProvider } from "@/contexts/EventContext";
import { EventNotification } from "@/components/EventNotification";
import { EventHandlerInit } from "@/components/EventHandlerInit";
import { useEffect } from "react";
import { applyTheme, getStoredTheme } from "@/lib/theme";

// Lazy load pages
const SignInPage = lazy(() => import("@/pages/signin/page"));
const DrivePage = lazy(() => import("@/pages/drive/page"));
const AdminPage = lazy(() => import("@/pages/admin/page"));
const ProfilePage = lazy(() => import("@/pages/profile/page"));

// ============================================================================
// PAGE LOADER
// ============================================================================

const PageLoader = () => (
    <div className="page-loader">
        <Loader2 className="page-loader-spinner" />
        <span className="page-loader-text">Loading Environment...</span>
    </div>
);

// ============================================================================
// PROTECTED ROUTE WRAPPER
// ============================================================================

interface ProtectedRouteProps {
    children: React.ReactNode;
    adminOnly?: boolean;
}

const ProtectedRoute = ({ children: t_children, adminOnly: t_admin_only = false }: ProtectedRouteProps) => {
    const { isAuthenticated, isLoading, user } = useAuth();

    if (isLoading) return <PageLoader />;

    if (!isAuthenticated) {
        return <Navigate to="/signin" replace />;
    }

    if (t_admin_only && user) {
        const isAdmin = typeof user.role === "string" ? user.role === "admin" : user.role?.name === "admin";
        if (!isAdmin) {
            return <Navigate to="/drive" replace />;
        }
    }

    return <>{t_children}</>;
};

// ============================================================================
// MAIN APP COMPONENT
// ============================================================================

function App() {
    useEffect(() => {
        applyTheme(getStoredTheme());
    }, []);

    return (
        <EventProvider>
            <AuthProvider>
                <EventHandlerInit />

                <Suspense fallback={<PageLoader />}>
                    <Routes>
                        {/* Public Routes */}
                        <Route path="/" element={<Navigate to="/signin" replace />} />
                        <Route path="/signin" element={<SignInPage />} />

                        <Route
                            path="/drive"
                            element={
                                <ProtectedRoute>
                                    <DrivePage />
                                </ProtectedRoute>
                            }
                        />

                        <Route
                            path="/profile"
                            element={
                                <ProtectedRoute>
                                    <ProfilePage />
                                </ProtectedRoute>
                            }
                        />

                        {/* Protected Admin Routes */}
                        <Route
                            path="/admin"
                            element={
                                <ProtectedRoute adminOnly>
                                    <AdminPage />
                                </ProtectedRoute>
                            }
                        />
                        {/* Catch all - redirect to signin */}
                        <Route path="*" element={<Navigate to="/signin" replace />} />
                    </Routes>
                </Suspense>

                <EventNotification />
            </AuthProvider>
        </EventProvider>
    );
}

export default App;
