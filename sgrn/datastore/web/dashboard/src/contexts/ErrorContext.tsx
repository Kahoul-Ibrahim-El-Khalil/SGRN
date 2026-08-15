import React, { createContext, useContext, useState, useCallback } from "react";

interface ErrorContextType {
    error: string | null;
    showError: (t_message: string) => void;
    clearError: () => void;
}

const ErrorContext = createContext<ErrorContextType | undefined>(undefined);

export function ErrorProvider({ children }: { children: React.ReactNode }) {
    const [error, setError] = useState<string | null>(null);

    const showError = useCallback((t_message: string) => {
        setError(t_message);
        // Auto-dismiss after 5 seconds
        setTimeout(() => setError(null), 5000);
    }, []);

    const clearError = useCallback(() => {
        setError(null);
    }, []);

    return <ErrorContext.Provider value={{ error, showError, clearError }}>{children}</ErrorContext.Provider>;
}

export function useError() {
    const context = useContext(ErrorContext);
    if (!context) {
        throw new Error("useError must be used within ErrorProvider");
    }
    return context;
}
