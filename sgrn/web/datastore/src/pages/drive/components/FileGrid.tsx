// @/pages/drive/components/FileGrid.tsx

import {
    Download,
    File,
    FileText,
    FileImage,
    Music,
    Archive,
    FolderPlus,
    Trash2,
    FileCode,
    FileSpreadsheet,
    Video,
    Info,
    Edit3,
    Eye,
} from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";
import { Highlight } from "./Highlight";
import React, { useState } from "react";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";

interface FileGridProps {
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

function getFileIcon(extension: string) {
    const ext = extension.toLowerCase();
    const imageExts = ["jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "ico"];
    const codeExts = ["js", "ts", "tsx", "jsx", "py", "cpp", "c", "h", "hpp", "css", "html", "json", "xml", "yaml", "yml"];
    const spreadsheetExts = ["csv", "xls", "xlsx", "ods"];
    const audioExts = ["mp3", "wav", "ogg", "flac", "aac", "m4a"];
    const videoExts = ["mp4", "avi", "mkv", "mov", "wmv", "webm"];
    const archiveExts = ["zip", "rar", "7z", "tar", "gz", "bz2", "xz"];

    if (imageExts.includes(ext)) return <FileImage size={16} className="file-grid-icon file-grid-icon--image" />;
    if (codeExts.includes(ext)) return <FileCode size={16} className="file-grid-icon file-grid-icon--code" />;
    if (spreadsheetExts.includes(ext)) return <FileSpreadsheet size={16} className="file-grid-icon file-grid-icon--sheet" />;
    if (audioExts.includes(ext)) return <Music size={16} className="file-grid-icon file-grid-icon--audio" />;
    if (videoExts.includes(ext)) return <Video size={16} className="file-grid-icon file-grid-icon--video" />;
    if (archiveExts.includes(ext)) return <Archive size={16} className="file-grid-icon file-grid-icon--archive" />;
    if (ext === "pdf") return <FileText size={16} className="file-grid-icon file-grid-icon--pdf" />;
    return <File size={16} className="file-grid-icon file-grid-icon--generic" />;
}

function getFolderLabel(folder: DriveFolder): string {
    return folder.display_name || folder.name;
}

export function FileGrid({
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
}: FileGridProps) {
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
            if (!targetFolder.id) return;
            if (draggedItem.type === "folder" && draggedItem.id === targetFolder.id) return;
            onMoveItems?.([draggedItem], targetFolder.id);
        } catch (err) {
            console.error("Drop failed", err);
        }
    };

    if (folders.length === 0 && files.length === 0) {
        return (
            <div className="drive-empty">
                <File size={48} strokeWidth={1} className="drive-empty-icon" />
                <p className="drive-empty-label">Directory is empty</p>
            </div>
        );
    }

    return (
        <div className="grid-industrial-drive">
            {/* Folders */}
            {folders.map((folder) => {
                const selected = folder.id ? isSelected({ id: folder.id, type: "folder" }) : false;
                const isDragOver = dragOverPath === folder.path;

                return (
                    <div
                        key={folder.path}
                        draggable={!!folder.id}
                        onDragStart={(e) =>
                            folder.id && handleDragStart(e, { id: folder.id, type: "folder", source_path: folder.path, name: folder.name })
                        }
                        onDragOver={(e) => {
                            e.preventDefault();
                            setDragOverPath(folder.path);
                        }}
                        onDragLeave={() => setDragOverPath(null)}
                        onDrop={(e) => handleDrop(e, folder)}
                        className={`file-card group${selected ? " file-card--selected" : ""}${isDragOver ? " file-card--drag-over" : ""}`}
                        onClick={() => onFolderClick(folder)}
                    >
                        {/* Top row: icon + checkbox */}
                        <div className="file-card-top">
                            <div className="file-card-icon-wrap">
                                <FolderPlus
                                    className={
                                        folder.token ? "file-grid-icon file-grid-icon--service" : "file-grid-icon file-grid-icon--folder"
                                    }
                                    size={16}
                                />
                            </div>
                            {folder.id && (
                                <div
                                    className="file-card-check"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onToggleSelection({
                                            id: folder.id!,
                                            type: "folder",
                                            source_path: folder.path,
                                            name: getFolderLabel(folder),
                                        });
                                    }}
                                >
                                    <input type="checkbox" checked={selected} readOnly className="checkbox-industrial" />
                                </div>
                            )}
                        </div>

                        {/* Name + type label */}
                        <div className="file-card-body">
                            <div className="file-card-name">
                                <Highlight text={getFolderLabel(folder)} query={searchQuery} />
                            </div>
                            <div className="file-card-type">{folder.token ? "INFRASTRUCTURE" : "DIRECTORY"}</div>
                        </div>

                        {/* Action buttons */}
                        <div className="file-card-actions">
                            {folder.id && onFolderDownload && (
                                <button
                                    className="file-card-action-btn"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onFolderDownload(folder);
                                    }}
                                >
                                    <Download size={11} />
                                </button>
                            )}
                            {folder.id && onInfo && (
                                <button
                                    className="file-card-action-btn"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onInfo(folder, "folder");
                                    }}
                                >
                                    <Info size={11} />
                                </button>
                            )}
                            {folder.id && onRename && (
                                <button
                                    className="file-card-action-btn"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onRename(folder, "folder");
                                    }}
                                >
                                    <Edit3 size={11} />
                                </button>
                            )}
                            {folder.id && onDelete && (
                                <button
                                    className="file-card-action-btn file-card-action-btn--danger"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onDelete(folder, "folder");
                                    }}
                                >
                                    <Trash2 size={11} />
                                </button>
                            )}
                        </div>
                    </div>
                );
            })}

            {/* Files */}
            {files.map((file) => {
                const selected = isSelected({ id: file.id, type: "file" });
                return (
                    <div
                        key={file.id}
                        draggable
                        onDragStart={(e) => handleDragStart(e, { id: file.id, type: "file", source_path: file.path, name: file.name })}
                        className={`file-card group${selected ? " file-card--selected" : ""}`}
                        onClick={() => onFileClick(file)}
                    >
                        <div className="file-card-top">
                            <div className="file-card-icon-wrap">{getFileIcon(file.extension)}</div>
                            <div
                                className="file-card-check"
                                onClick={(e) => {
                                    e.stopPropagation();
                                    onToggleSelection({ id: file.id, type: "file", source_path: file.path, name: file.name });
                                }}
                            >
                                <input type="checkbox" checked={selected} readOnly className="checkbox-industrial" />
                            </div>
                        </div>

                        <div className="file-card-body">
                            <div className="file-card-name">
                                <Highlight text={file.name} query={searchQuery} />
                            </div>
                            <div className="file-card-meta">
                                <span className="file-card-type">{file.extension || "DATA"}</span>
                                <span className="file-card-size">{formatFileSize(file.size)}</span>
                            </div>
                        </div>

                        <div className="file-card-actions">
                            {onPreview && (
                                <button
                                    className="file-card-action-btn"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onPreview(file);
                                    }}
                                >
                                    <Eye size={11} />
                                </button>
                            )}
                            <button
                                className="file-card-action-btn"
                                onClick={(e) => {
                                    e.stopPropagation();
                                    onInfo?.(file, "file");
                                }}
                            >
                                <Info size={11} />
                            </button>
                            <button
                                className="file-card-action-btn"
                                onClick={(e) => {
                                    e.stopPropagation();
                                    onRename?.(file, "file");
                                }}
                            >
                                <Edit3 size={11} />
                            </button>

                            {file.name.endsWith(".zst") && onDownloadDecompressed ? (
                                <div onClick={(e) => e.stopPropagation()}>
                                    <DropdownMenu>
                                        <DropdownMenuTrigger asChild>
                                            <button className="file-card-action-btn">
                                                <Download size={11} />
                                            </button>
                                        </DropdownMenuTrigger>
                                        <DropdownMenuContent align="end" className="w-48 bg-[#252526] border-white/10 text-white shadow-xl">
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
                                    className="file-card-action-btn"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onFileClick(file);
                                    }}
                                >
                                    <Download size={11} />
                                </button>
                            )}
                            {onDelete && (
                                <button
                                    className="file-card-action-btn file-card-action-btn--danger"
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onDelete?.(file, "file");
                                    }}
                                >
                                    <Trash2 size={11} />
                                </button>
                            )}
                        </div>
                    </div>
                );
            })}
        </div>
    );
}
