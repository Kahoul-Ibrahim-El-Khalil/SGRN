import type { RefObject } from "react";
import { FolderPlus, Loader2 } from "lucide-react";

interface NewFolderPromptProps {
    inputRef: RefObject<HTMLInputElement | null>;
    name: string;
    creating: boolean;
    onChange: (name: string) => void;
    onSubmit: () => void;
    onCancel: () => void;
}

export function NewFolderPrompt({ inputRef, name, creating, onChange, onSubmit, onCancel }: NewFolderPromptProps) {
    return (
        <form
            className="drive-new-folder-prompt"
            onSubmit={(event) => {
                event.preventDefault();
                onSubmit();
            }}
        >
            <FolderPlus size={16} />
            <input
                ref={inputRef}
                type="text"
                className="input-desktop"
                placeholder="New folder name..."
                value={name}
                onChange={(event) => onChange(event.currentTarget.value)}
                disabled={creating}
                aria-label="New folder name"
                autoComplete="off"
            />
            <button type="submit" className="btn-desktop-primary" disabled={creating || !name.trim()}>
                {creating ? <Loader2 size={14} className="drive-spin" /> : "Create"}
            </button>
            <button type="button" className="btn-desktop-secondary" onClick={onCancel} disabled={creating}>
                Cancel
            </button>
        </form>
    );
}
