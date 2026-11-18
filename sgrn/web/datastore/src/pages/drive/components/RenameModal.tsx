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
        <div className="rename-modal-overlay" onClick={onClose}>
            <div className="rename-modal" onClick={(e) => e.stopPropagation()}>
                <div className="rename-modal-header">
                    <div className="rename-modal-icon">
                        <Edit3 size={20} />
                    </div>
                    <h2 className="rename-modal-title">Rename</h2>
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
                    className="rename-modal-input"
                    placeholder="Enter new name..."
                />

                <div className="rename-modal-actions">
                    <button type="button" onClick={onClose} disabled={loading} className="rename-modal-btn">
                        Cancel
                    </button>
                    <button
                        type="button"
                        onClick={handleConfirm}
                        disabled={loading || !newName.trim() || newName === initialName}
                        className="rename-modal-btn rename-modal-btn--primary"
                    >
                        {loading ? <Loader2 size={18} className="drive-spin" /> : <Check size={18} />}
                        Save Changes
                    </button>
                </div>
            </div>
        </div>
    );
};
