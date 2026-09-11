import React, { createContext, useContext, useState, useEffect, useCallback } from "react";

import type { User } from "@/pages/signin/types"; // Assumes you have the User type defined in types/index.ts

interface AuthContextType {
    user: User | null;
    token: string | null;
    isAuthenticated: boolean;
    isLoading: boolean;
    login: (t_user_data: User, t_token: string) => void;
    logout: () => void;
}

const AuthContext = createContext<AuthContextType | undefined>(undefined);

export function AuthProvider({ children }: { children: React.ReactNode }) {
    const [user, setUser] = useState<User | null>(null);
    const [token, setToken] = useState<string | null>(null);
    const [isLoading, setIsLoading] = useState(true);

    // Initialize state from storage on application load
    useEffect(() => {
        try {
            const storedUser = sessionStorage.getItem("user_info");
            const storedToken = sessionStorage.getItem("SGRN-TOKEN");

            if (storedUser && storedToken) {
                setUser(JSON.parse(storedUser));
                setToken(storedToken);
            }
        } catch (error) {
            console.error("Failed to restore authentication state:", error);
            sessionStorage.removeItem("user_info");
            sessionStorage.removeItem("SGRN-TOKEN");
        } finally {
            setIsLoading(false);
        }
    }, []);

    const login = useCallback((t_user_data: User, t_token: string) => {
        setUser(t_user_data);
        setToken(t_token);

        sessionStorage.setItem("user_info", JSON.stringify(t_user_data));
        sessionStorage.setItem("SGRN-TOKEN", t_token);
    }, []);

    const logout = useCallback(() => {
        setUser(null);
        setToken(null);

        sessionStorage.removeItem("user_info");
        sessionStorage.removeItem("SGRN-TOKEN");
        localStorage.removeItem("user_info");
        localStorage.removeItem("SGRN-TOKEN");

        window.location.href = "/signin";
    }, []);

    return (
        <AuthContext.Provider value={{ user, token, isAuthenticated: !!user, isLoading, login, logout }}>{children}</AuthContext.Provider>
    );
}

export function useAuth() {
    const context = useContext(AuthContext);
    if (context === undefined) {
        throw new Error("useAuth must be used within an AuthProvider");
    }
    return context;
}
