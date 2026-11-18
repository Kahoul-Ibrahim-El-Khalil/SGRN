import { Lock } from "lucide-react";
import type { DriveFile } from "@sgrn/types";
import type { DriveItem, DriveMoveItem } from "@/pages/drive/types";
import type { DragEvent } from "react";
import { Highlight } from "../Highlight";
import { DriveFileIcon } from "../DriveFileIcon";
import { formatDate, formatFileSize, toFileMoveItem } from "./format";
import { FileListFileActions } from "./FileListFileActions";

interface FileListFileRowProps {
    file: DriveFile;
    searchQuery: string;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelection: (item: DriveMoveItem) => void;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete?: (item: DriveItem, type: "file" | "folder") => void;
    onRename?: (item: DriveItem, type: "file" | "folder") => void;
    onInfo?: (item: DriveItem, type: "file" | "folder") => void;
    canMove: boolean;
    onDragStart: (event: DragEvent<HTMLTableRowElement>, item: DriveMoveItem) => void;
    onDragEnd: () => void;
}

export function FileListFileRow({
    file,
    searchQuery,
    isSelected,
    onToggleSelection,
    onFileClick,
    onDownloadDecompressed,
    onPreview,
    onDelete,
    onRename,
    onInfo,
    canMove,
    onDragStart,
    onDragEnd,
}: FileListFileRowProps) {
    const moveItem = toFileMoveItem(file);
    // Keep the same stable selection key used by the original list implementation.
    const selected = isSelected({ id: file.id, type: "file" });

    return (
        <tr
            draggable={canMove}
            onDragStart={(event) => onDragStart(event, moveItem)}
            onDragEnd={onDragEnd}
            className={`group cursor-pointer transition-colors ${selected ? "active" : ""}`}
            onClick={() => onFileClick(file)}
            tabIndex={0}
            aria-selected={selected}
            onKeyDown={(event) => {
                // Only react when the row itself is focused, not a nested button.
                if (event.target !== event.currentTarget) return;
                if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    onFileClick(file);
                }
            }}
        >
            <td className="text-center" onClick={(event) => event.stopPropagation()}>
                <input
                    type="checkbox"
                    checked={selected}
                    className="checkbox-industrial"
                    aria-label={`Select file ${file.name}`}
                    onChange={() => onToggleSelection(moveItem)}
                    onClick={(event) => event.stopPropagation()}
                />
            </td>
            <td>
                <DriveFileIcon extension={file.extension} variant="list" />
            </td>
            <td className="font-bold whitespace-nowrap">
                <div className="flex items-center gap-2">
                    <Highlight text={file.name} query={searchQuery} />
                    {file.is_encrypted && (
                        <span
                            className="inline-flex items-center gap-1 px-1.5 py-0.5 rounded text-[9px] font-mono font-semibold bg-amber-500/10 text-amber-500 border border-amber-500/20"
                            title={`Encrypted at rest (Key: ${file.key_id || "AES-256-GCM"})`}
                        >
                            <Lock size={10} className="stroke-[2.5]" />
                            <span>ENC</span>
                        </span>
                    )}
                </div>
            </td>
            <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-wider">{file.extension || "DATA"}</td>
            <td className="font-mono text-[10px] text-muted-foreground">{formatFileSize(file.size)}</td>
            <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-tighter">{formatDate(file.created_at)}</td>
            <td>
                <FileListFileActions
                    file={file}
                    onFileClick={onFileClick}
                    onDownloadDecompressed={onDownloadDecompressed}
                    onPreview={onPreview}
                    onDelete={onDelete}
                    onRename={onRename}
                    onInfo={onInfo}
                />
            </td>
        </tr>
    );
}
