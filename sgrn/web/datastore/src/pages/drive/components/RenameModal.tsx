import React, { useState, useEffect, useRef } from "react";
import { Check, Loader2, Edit3 } from "lucide-react";

interface RenameModalProps {
    isOpen: boolean;
    initialName: string;
    onClose: () => void;
    onConfirm: (newName: string) => Promise<boolean>;
}

export const RenameModal: React.FC<RenameModalProps> = ({ isOpen, initialName, onClose, onConfirm }) => {
    const [newName, setNewName] = useState(initialName);
    const [loading, setLoading] = useState(false);
    const inputRef = useRef<HTMLInputElement>(null);

    useEffect(() => {
        if (isOpen) {
            setNewName(initialName);
            setTimeout(() => {
                inputRef.current?.focus();
                inputRef.current?.select();
            }, 100);
        }
    }, [isOpen, initialName]);

    if (!isOpen) return null;

    const handleConfirm = async () => {
        if (loading) return;
        if (!newName.trim() || newName === initialName) {
            onClose();
            return;
        }
        setLoading(true);
        try {
            const success = await onConfirm(newName.trim());
            if (success) onClose();
        } finally {
            setLoading(false);
        }
    };

    return (
        <div
            style={{
                position: "fixed",
                inset: 0,
                backgroundColor: "rgba(0, 0, 0, 0.6)",
                display: "flex",
                alignItems: "center",
                justifyContent: "center",
                zIndex: 1000,
                backdropFilter: "blur(4px)",
            }}
            onClick={onClose}
        >
            <div
                style={{
                    backgroundColor: "hsl(var(--card))",
                    borderRadius: "0", // Enforce industrial hard corners
                    width: "400px",
                    maxWidth: "90%",
                    padding: "1.5rem",
                    boxShadow: "var(--shadow-lg)",
                    border: "1px solid hsl(var(--border))",
                    color: "hsl(var(--foreground))",
                }}
                onClick={(e) => e.stopPropagation()}
            >
                <div style={{ display: "flex", alignItems: "center", gap: "0.75rem", marginBottom: "1.25rem" }}>
                    <div
                        style={{
                            backgroundColor: "hsl(var(--primary) / 0.1)",
                            color: "hsl(var(--primary))",
                            padding: "0.5rem",
                            borderRadius: "0",
                        }}
                    >
                        <Edit3 size={20} />
                    </div>
                    <h2 style={{ fontSize: "1.25rem", margin: 0 }}>Rename</h2>
                </div>

                <input
                    ref={inputRef}
                    type="text"
                    value={newName}
                    onChange={(e) => setNewName(e.target.value)}
                    onKeyDown={(e) => {
                        if (e.key === "Enter") handleConfirm();
                        if (e.key === "Escape") onClose();
                    }}
                    style={{
                        width: "100%",
                        backgroundColor: "hsl(var(--background) / 0.5)",
                        border: "1px solid hsl(var(--border))",
                        borderRadius: "0", // Enforce industrial hard corners
                        padding: "0.75rem 1rem",
                        color: "hsl(var(--foreground))",
                        fontSize: "1rem",
                        outline: "none",
                        marginBottom: "1.5rem",
                    }}
                    placeholder="Enter new name..."
                />

                <div style={{ display: "flex", gap: "0.75rem", justifyContent: "flex-end" }}>
                    <button
                        onClick={onClose}
                        disabled={loading}
                        style={{
                            padding: "0.6rem 1.25rem",
                            borderRadius: "0", // Enforce industrial hard corners
                            border: "1px solid hsl(var(--border))",
                            backgroundColor: "hsl(var(--secondary) / 0.5)",
                            color: "hsl(var(--muted-foreground))",
                            cursor: "pointer",
                            fontSize: "0.9rem",
                            fontWeight: 500,
                        }}
                    >
                        Cancel
                    </button>
                    <button
                        onClick={handleConfirm}
                        disabled={loading || !newName.trim() || newName === initialName}
                        style={{
                            padding: "0.6rem 1.25rem",
                            borderRadius: "0.75rem",
                            border: "none",
                            backgroundColor: "#3b82f6",
                            color: "#fff",
                            cursor: "pointer",
                            fontSize: "0.9rem",
                            fontWeight: 600,
                            display: "flex",
                            alignItems: "center",
                            gap: "0.5rem",
                            opacity: loading || !newName.trim() || newName === initialName ? 0.6 : 1,
                        }}
                    >
                        {loading ? <Loader2 size={18} className="drive-spin" /> : <Check size={18} />}
                        Save Changes
                    </button>
                </div>
            </div>
        </div>
    );
};
