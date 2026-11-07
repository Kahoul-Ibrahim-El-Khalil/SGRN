// @/pages/drive/components/FileList.tsx

import {
    FileText,
    Download,
    FileImage,
    FileCode,
    FileSpreadsheet,
    Music,
    Video,
    Archive,
    File,
    Trash2,
    FolderPlus,
    Info,
    Edit3,
    Eye,
    ArrowUp,
    ArrowDown,
    Lock,
} from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";
import { Highlight } from "./Highlight";
import type { DriveSortKey, DriveSortOrder } from "@/pages/drive/api";
import React, { useState } from "react";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";

interface FileListProps {
    folders: DriveFolder[];
    files: DriveFile[];
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete?: (item: any, type: "file" | "folder") => void;
    onRename?: (item: any, type: "file" | "folder") => void;
    onInfo?: (item: any, type: "file" | "folder") => void;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelection: (item: DriveMoveItem) => void;
    onMoveItems?: (items: DriveMoveItem[], targetId: number) => void;
    searchQuery?: string;
    sortBy?: DriveSortKey;
    sortOrder?: DriveSortOrder;
    onSort?: (key: DriveSortKey) => void;
}

function formatFileSize(bytes: number): string {
    if (bytes === 0) return "0 B";
    const units = ["B", "KB", "MB", "GB", "TB"];
    const i = Math.floor(Math.log(bytes) / Math.log(1024));
    return `${(bytes / Math.pow(1024, i)).toFixed(i === 0 ? 0 : 1)} ${units[i]}`;
}

function formatDate(dateStr: string): string {
    try {
        const date = new Date(dateStr);
        return date.toLocaleDateString(undefined, {
            year: "numeric",
            month: "short",
            day: "numeric",
            hour: "2-digit",
            minute: "2-digit",
        });
    } catch {
        return dateStr;
    }
}

function getFileIcon(extension: string) {
    const ext = extension.toLowerCase();
    const imageExts = ["jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "ico"];
    const codeExts = ["js", "ts", "tsx", "jsx", "py", "cpp", "c", "h", "hpp", "css", "html", "json", "xml", "yaml", "yml"];
    const spreadsheetExts = ["csv", "xls", "xlsx", "ods"];
    const audioExts = ["mp3", "wav", "ogg", "flac", "aac", "m4a"];
    const videoExts = ["mp4", "avi", "mkv", "mov", "wmv", "webm"];
    const archiveExts = ["zip", "rar", "7z", "tar", "gz", "bz2", "xz"];

    if (imageExts.includes(ext)) return <FileImage size={18} className="drive-file-icon drive-icon-image" />;
    if (codeExts.includes(ext)) return <FileCode size={18} className="drive-file-icon drive-icon-code" />;
    if (spreadsheetExts.includes(ext)) return <FileSpreadsheet size={18} className="drive-file-icon drive-icon-spreadsheet" />;
    if (audioExts.includes(ext)) return <Music size={18} className="drive-file-icon drive-icon-audio" />;
    if (videoExts.includes(ext)) return <Video size={18} className="drive-file-icon drive-icon-video" />;
    if (archiveExts.includes(ext)) return <Archive size={18} className="drive-file-icon drive-icon-archive" />;
    if (ext === "pdf") return <FileText size={18} className="drive-file-icon drive-icon-pdf" />;
    return <File size={18} className="drive-file-icon drive-icon-generic" />;
}

function getFolderLabel(folder: DriveFolder): string {
    if (folder.token && folder.display_name) {
        return folder.display_name;
    }
    return folder.display_name || folder.name;
}

export function FileList({
    folders,
    files,
    onFolderClick,
    onFolderDownload,
    onFileClick,
    onDownloadDecompressed,
    onPreview,
    onDelete,
    onRename,
    onInfo,
    isSelected,
    onToggleSelection,
    onMoveItems,
    searchQuery = "",
    sortBy = "name",
    sortOrder = "asc",
    onSort,
}: FileListProps) {
    // Sortable column headers; a click on the active column flips direction
    // (handled by the parent's toggleSort), a new column starts ascending.
    const SortHeader: React.FC<{ label: string; sortKey: DriveSortKey; className?: string }> = ({ label, sortKey, className }) => (
        <th className={className}>
            {onSort ? (
                <button
                    type="button"
                    onClick={(e) => {
                        e.stopPropagation();
                        onSort(sortKey);
                    }}
                    className={`drive-list-head-btn${sortBy === sortKey ? " drive-list-head-btn--active" : ""}`}
                    aria-label={`Sort by ${label}${sortBy === sortKey ? (sortOrder === "asc" ? ", currently ascending" : ", currently descending") : ""}`}
                >
                    <span>{label}</span>
                    {sortBy === sortKey && (sortOrder === "asc" ? <ArrowUp size={10} /> : <ArrowDown size={10} />)}
                </button>
            ) : (
                label
            )}
        </th>
    );
    const [dragOverPath, setDragOverPath] = useState<string | null>(null);

    const handleDragStart = (e: React.DragEvent, item: DriveMoveItem) => {
        e.dataTransfer.setData("application/json", JSON.stringify(item));
        e.dataTransfer.effectAllowed = "move";
    };

    const handleDrop = (e: React.DragEvent, targetFolder: DriveFolder) => {
        e.preventDefault();
        setDragOverPath(null);
        try {
            const data = e.dataTransfer.getData("application/json");
            if (!data) return;
            const draggedItem = JSON.parse(data) as DriveMoveItem;

            // Don't drop on itself or virtual folders without ID
            if (!targetFolder.id) return;
            if (draggedItem.type === "folder" && draggedItem.id === targetFolder.id) return;

            onMoveItems?.([draggedItem], targetFolder.id);
        } catch (err) {
            console.error("Drop failed", err);
        }
    };

    const isEmpty = folders.length === 0 && files.length === 0;

    // Separators only earn their place when there are enough rows to need them
    const showSeparators = folders.length + files.length > 10;

    if (isEmpty) {
        return (
            <div className="drive-empty">
                <div className="p-8 border-2 border-dashed border-border/20 rounded-lg flex flex-col items-center gap-4">
                    <File size={48} strokeWidth={1} className="text-muted-foreground/20" />
                    <p className="text-[10px] font-black uppercase tracking-[0.3em]">Directory is empty</p>
                </div>
            </div>
        );
    }

    return (
        <div className="overflow-x-auto">
            <table className={`drive-list w-full${showSeparators ? " drive-list--lined" : ""}`}>
                <thead>
                    <tr>
                        <th aria-label="Selection" />
                        <th aria-label="Type icon" />
                        <SortHeader label="Name" sortKey="name" />
                        <th>Type</th>
                        <SortHeader label="Size" sortKey="size" />
                        <SortHeader label="Modified" sortKey="modified" />
                        <th aria-label="Actions" />
                    </tr>
                </thead>
                <tbody>
                    {/* Folders first */}
                    {folders.map((folder) => {
                        const selected = folder.id ? isSelected({ id: folder.id, type: "folder" }) : false;
                        const isDragOver = dragOverPath === folder.path;

                        return (
                            <tr
                                key={folder.path}
                                draggable={!!folder.id}
                                onDragStart={(e) =>
                                    folder.id &&
                                    handleDragStart(e, { id: folder.id, type: "folder", source_path: folder.path, name: folder.name })
                                }
                                onDragOver={(e) => {
                                    e.preventDefault();
                                    setDragOverPath(folder.path);
                                }}
                                onDragLeave={() => setDragOverPath(null)}
                                onDrop={(e) => handleDrop(e, folder)}
                                className={`group cursor-pointer transition-colors ${selected ? "active" : ""} ${isDragOver ? "drop-target" : ""}`}
                                onClick={() => onFolderClick(folder)}
                            >
                                <td className="text-center" onClick={(e) => e.stopPropagation()}>
                                    {folder.id && (
                                        <input
                                            type="checkbox"
                                            checked={selected}
                                            readOnly
                                            className="checkbox-industrial"
                                            onClick={() =>
                                                onToggleSelection({
                                                    id: folder.id!,
                                                    type: "folder",
                                                    source_path: folder.path,
                                                    name: getFolderLabel(folder),
                                                })
                                            }
                                        />
                                    )}
                                </td>
                                <td>
                                    <FolderPlus size={16} className={folder.token ? "text-amber-500" : "text-primary"} />
                                </td>
                                <td className="font-bold whitespace-nowrap">
                                    <Highlight text={getFolderLabel(folder)} query={searchQuery} />
                                </td>
                                <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-wider">
                                    {folder.token ? "SERVICE" : "FOLDER"}
                                </td>
                                <td className="font-mono text-[10px] text-muted-foreground">
                                    {typeof folder.virtual_size === "number" ? formatFileSize(folder.virtual_size) : "--"}
                                </td>
                                <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-tighter">
                                    {folder.token
                                        ? "INFRASTRUCTURE"
                                        : (folder as any).created_at
                                          ? formatDate((folder as any).created_at)
                                          : "--"}
                                </td>
                                <td>
                                    <div className="drive-row-actions opacity-0 group-hover:opacity-100 transition-opacity">
                                        {folder.id && onFolderDownload && (
                                            <button
                                                className="drive-row-btn"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onFolderDownload(folder);
                                                }}
                                            >
                                                <Download size={12} />
                                            </button>
                                        )}
                                        {folder.id && onInfo && (
                                            <button
                                                className="drive-row-btn"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onInfo(folder, "folder");
                                                }}
                                            >
                                                <Info size={12} />
                                            </button>
                                        )}
                                        {folder.id && onRename && (
                                            <button
                                                className="drive-row-btn"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onRename(folder, "folder");
                                                }}
                                            >
                                                <Edit3 size={12} />
                                            </button>
                                        )}
                                        {folder.id && onDelete && (
                                            <button
                                                className="drive-row-btn drive-row-btn--danger"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onDelete(folder, "folder");
                                                }}
                                            >
                                                <Trash2 size={12} />
                                            </button>
                                        )}
                                    </div>
                                </td>
                            </tr>
                        );
                    })}

                    {/* Then files */}
                    {files.map((file) => {
                        const selected = isSelected({ id: file.id, type: "file" });
                        return (
                            <tr
                                key={file.id}
                                draggable
                                onDragStart={(e) =>
                                    handleDragStart(e, { id: file.id, type: "file", source_path: file.path, name: file.name })
                                }
                                className={`group cursor-pointer transition-colors ${selected ? "active" : ""}`}
                                onClick={() => onFileClick(file)}
                            >
                                <td className="text-center" onClick={(e) => e.stopPropagation()}>
                                    <input
                                        type="checkbox"
                                        checked={selected}
                                        readOnly
                                        className="checkbox-industrial"
                                        onClick={() =>
                                            onToggleSelection({ id: file.id, type: "file", source_path: file.path, name: file.name })
                                        }
                                    />
                                </td>
                                <td>{getFileIcon(file.extension)}</td>
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
                                <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-wider">
                                    {file.extension || "DATA"}
                                </td>
                                <td className="font-mono text-[10px] text-muted-foreground">{formatFileSize(file.size)}</td>
                                <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-tighter">
                                    {formatDate(file.created_at)}
                                </td>
                                <td>
                                    <div className="drive-row-actions opacity-0 group-hover:opacity-100 transition-opacity">
                                        {onPreview && (
                                            <button
                                                className="drive-row-btn"
                                                title="Preview"
                                                aria-label="Preview"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onPreview(file);
                                                }}
                                            >
                                                <Eye size={12} />
                                            </button>
                                        )}
                                        <button
                                            className="drive-row-btn"
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                onInfo?.(file, "file");
                                            }}
                                        >
                                            <Info size={12} />
                                        </button>
                                        <button
                                            className="drive-row-btn"
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                onRename?.(file, "file");
                                            }}
                                        >
                                            <Edit3 size={12} />
                                        </button>

                                        {file.name.endsWith(".zst") && onDownloadDecompressed ? (
                                            <div onClick={(e) => e.stopPropagation()}>
                                                <DropdownMenu>
                                                    <DropdownMenuTrigger asChild>
                                                        <button className="drive-row-btn">
                                                            <Download size={12} />
                                                        </button>
                                                    </DropdownMenuTrigger>
                                                    <DropdownMenuContent
                                                        align="end"
                                                        className="w-48 bg-popover border-border text-popover-foreground shadow-xl"
                                                    >
                                                        <DropdownMenuItem
                                                            className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                                                            onClick={() => onDownloadDecompressed(file)}
                                                        >
                                                            Download Decompressed
                                                        </DropdownMenuItem>
                                                        <DropdownMenuItem
                                                            className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                                                            onClick={() => onFileClick(file)}
                                                        >
                                                            Download Original (.zst)
                                                        </DropdownMenuItem>
                                                    </DropdownMenuContent>
                                                </DropdownMenu>
                                            </div>
                                        ) : (
                                            <button
                                                className="drive-row-btn"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onFileClick(file);
                                                }}
                                            >
                                                <Download size={12} />
                                            </button>
                                        )}
                                        {onDelete && (
                                            <button
                                                className="drive-row-btn drive-row-btn--danger"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onDelete?.(file, "file");
                                                }}
                                            >
                                                <Trash2 size={12} />
                                            </button>
                                        )}
                                    </div>
                                </td>
                            </tr>
                        );
                    })}
                </tbody>
            </table>
        </div>
    );
}
