import React from "react";
import { X, Trash2, Move, CheckSquare } from "lucide-react";

interface SelectionBarProps {
    selectedCount: number;
    onClear: () => void;
    onDelete: () => void;
    onMove: () => void;
}

export const SelectionBar: React.FC<SelectionBarProps> = ({ selectedCount, onClear, onDelete, onMove }) => {
    if (selectedCount === 0) return null;

    return (
        <div className="drive-selection-bar">
            <div className="flex items-center gap-4 pr-6 border-r border-border/30">
                <CheckSquare size={18} className="text-primary" />
                <span className="text-[11px] font-black uppercase tracking-widest">{selectedCount} SELECTED</span>
            </div>

            <div className="flex gap-2">
                <button onClick={onMove} className="btn-industrial-secondary gap-2 h-9 px-4 hover:text-primary transition-colors">
                    <Move size={14} /> MOVE
                </button>
                <button
                    onClick={onDelete}
                    className="btn-industrial-secondary gap-2 h-9 px-4 border-destructive/20 text-destructive hover:bg-destructive/10"
                >
                    <Trash2 size={14} /> DELETE
                </button>
            </div>

            <button
                onClick={onClear}
                className="p-1 text-muted-foreground/40 hover:text-foreground transition-colors ml-2"
                title="Clear selection"
            >
                <X size={20} />
            </button>
        </div>
    );
};
