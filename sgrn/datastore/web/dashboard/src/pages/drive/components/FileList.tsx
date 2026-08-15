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
} from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";
import { Highlight } from "./Highlight";
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
}: FileListProps) {
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
            <table className="datagrid-industrial w-full">
                <thead>
                    <tr>
                        <th className="w-10"></th>
                        <th className="w-10"></th>
                        <th>NAME</th>
                        <th className="w-24">TYPE</th>
                        <th className="w-32">SIZE</th>
                        <th className="w-48">MODIFIED</th>
                        <th className="w-40 text-right">ACTIONS</th>
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
                                className={`group cursor-pointer transition-colors ${selected ? "active" : "hover:bg-muted/10"} ${isDragOver ? "drop-target" : ""}`}
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
                                    <div className="flex justify-end gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
                                        {folder.id && onFolderDownload && (
                                            <button
                                                className="btn-industrial-secondary h-7 w-7 p-0"
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
                                                className="btn-industrial-secondary h-7 w-7 p-0"
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
                                                className="btn-industrial-secondary h-7 w-7 p-0"
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
                                                className="btn-industrial-secondary h-7 w-7 p-0 border-destructive/20 text-destructive hover:bg-destructive/10"
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
                                className={`group cursor-pointer transition-colors ${selected ? "active" : "hover:bg-muted/10"}`}
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
                                    <Highlight text={file.name} query={searchQuery} />
                                </td>
                                <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-wider">
                                    {file.extension || "DATA"}
                                </td>
                                <td className="font-mono text-[10px] text-muted-foreground">{formatFileSize(file.size)}</td>
                                <td className="text-[10px] font-mono text-muted-foreground uppercase tracking-tighter">
                                    {formatDate(file.created_at)}
                                </td>
                                <td>
                                    <div className="flex justify-end gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
                                        {onPreview && (
                                            <button
                                                className="btn-industrial-secondary h-7 w-7 p-0"
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    onPreview(file);
                                                }}
                                            >
                                                <Eye size={12} />
                                            </button>
                                        )}
                                        <button
                                            className="btn-industrial-secondary h-7 w-7 p-0"
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                onInfo?.(file, "file");
                                            }}
                                        >
                                            <Info size={12} />
                                        </button>
                                        <button
                                            className="btn-industrial-secondary h-7 w-7 p-0"
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
                                                        <button className="btn-industrial-secondary h-7 w-7 p-0">
                                                            <Download size={12} />
                                                        </button>
                                                    </DropdownMenuTrigger>
                                                    <DropdownMenuContent
                                                        align="end"
                                                        className="w-48 bg-[#252526] border-white/10 text-white shadow-xl"
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
                                                className="btn-industrial-secondary h-7 w-7 p-0"
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
                                                className="btn-industrial-secondary h-7 w-7 p-0 border-destructive/20 text-destructive hover:bg-destructive/10"
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
