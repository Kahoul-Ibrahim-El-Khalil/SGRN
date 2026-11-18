import { FolderPlus } from "lucide-react";
import type { DriveFolder } from "@sgrn/types";
import type { DriveItem, DriveMoveItem } from "@/pages/drive/types";
import { Highlight } from "../Highlight";
import type { DragEvent } from "react";
import { formatDate, formatFileSize, getFolderLabel, toFolderMoveItem } from "./format";
import { FileListFolderActions } from "./FileListFolderActions";

interface FileListFolderRowProps {
    folder: DriveFolder;
    searchQuery: string;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelection: (item: DriveMoveItem) => void;
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onDelete?: (item: DriveItem, type: "file" | "folder") => void;
    onRename?: (item: DriveItem, type: "file" | "folder") => void;
    onInfo?: (item: DriveItem, type: "file" | "folder") => void;
    canMove: boolean;
    isDragOver: boolean;
    onDragStart: (event: DragEvent<HTMLTableRowElement>, item: DriveMoveItem) => void;
    onDragEnd: () => void;
    onDragOver: (event: DragEvent<HTMLTableRowElement>, folder: DriveFolder) => void;
    onDragLeave: (event: DragEvent<HTMLTableRowElement>, path: string) => void;
    onDrop: (event: DragEvent<HTMLTableRowElement>, folder: DriveFolder) => void;
}

export function FileListFolderRow({
    folder,
    searchQuery,
    isSelected,
    onToggleSelection,
    onFolderClick,
    onFolderDownload,
    onDelete,
    onRename,
    onInfo,
    canMove,
    isDragOver,
    onDragStart,
    onDragEnd,
    onDragOver,
    onDragLeave,
    onDrop,
}: FileListFolderRowProps) {
    const moveItem = toFolderMoveItem(folder);
    const selected = moveItem !== null && isSelected(moveItem);
    const label = getFolderLabel(folder);
    const canSelect = moveItem !== null;

    return (
        <tr
            draggable={canSelect && canMove}
            onDragStart={moveItem ? (event) => onDragStart(event, moveItem) : undefined}
            onDragEnd={onDragEnd}
            onDragOver={(event) => onDragOver(event, folder)}
            onDragLeave={(event) => onDragLeave(event, folder.path)}
            onDrop={(event) => onDrop(event, folder)}
            className={`group cursor-pointer transition-colors ${selected ? "active" : ""} ${isDragOver ? "drop-target" : ""}`}
            onClick={() => onFolderClick(folder)}
            tabIndex={0}
            aria-selected={selected}
            onKeyDown={(event) => {
                // Only react when the row itself is focused, not a nested button.
                if (event.target !== event.currentTarget) return;
                if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    onFolderClick(folder);
                }
            }}
        >
            <td className="text-center" onClick={(event) => event.stopPropagation()}>
                {moveItem !== null && (
                    <input
                        type="checkbox"
                        checked={selected}
                        className="checkbox-industrial"
                        aria-label={`Select folder ${label}`}
                        onChange={() => onToggleSelection(moveItem)}
                        onClick={(event) => event.stopPropagation()}
                    />
                )}
            </td>
            <td>
                <FolderPlus size={16} className={folder.token ? "text-amber-500" : "text-primary"} />
            </td>
            <td className="font-bold whitespace-nowrap">
                <Highlight text={label} query={searchQuery} />
            </td>
            <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-wider">{folder.token ? "SERVICE" : "FOLDER"}</td>
            <td className="font-mono text-[10px] text-muted-foreground">
                {typeof folder.virtual_size === "number" ? formatFileSize(folder.virtual_size) : "--"}
            </td>
            <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-tighter">
                {folder.token
                    ? "INFRASTRUCTURE"
                    : formatDate("created_at" in folder && typeof folder.created_at === "string" ? folder.created_at : "")}
            </td>
            <td>
                {canSelect && (
                    <FileListFolderActions
                        folder={folder}
                        onFolderDownload={onFolderDownload}
                        onInfo={onInfo}
                        onRename={onRename}
                        onDelete={onDelete}
                    />
                )}
            </td>
        </tr>
    );
}
