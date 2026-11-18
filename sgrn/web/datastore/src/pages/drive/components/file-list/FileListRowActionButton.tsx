import type { LucideIcon } from "lucide-react";

interface FileListRowActionButtonProps {
    icon: LucideIcon;
    label: string;
    onClick: () => void;
    danger?: boolean;
}

export function FileListRowActionButton({ icon: Icon, label, onClick, danger = false }: FileListRowActionButtonProps) {
    return (
        <button
            type="button"
            className={`drive-row-btn${danger ? " drive-row-btn--danger" : ""}`}
            title={label}
            aria-label={label}
            onClick={(event) => {
                event.stopPropagation();
                onClick();
            }}
        >
            <Icon size={12} />
        </button>
    );
}
