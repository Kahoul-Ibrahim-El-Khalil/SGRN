// @/pages/drive/components/FileGrid.tsx

import React, { useRef, useState } from "react";
import {
    Archive,
    Download,
    Edit3,
    Eye,
    File as FileIcon,
    FileCode,
    FileImage,
    FileSpreadsheet,
    FileText,
    Folder,
    History,
    Info,
    Loader2,
    Lock,
    Music,
    Trash2,
    Video,
} from "lucide-react";
import type { LucideIcon } from "lucide-react";
import type { DriveFile, DriveFolder } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";
import type { DriveSortKey, DriveSortOrder } from "@/pages/drive/api";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";
import { Highlight } from "./Highlight";

export type DriveGridItem = DriveFile | DriveFolder;
export type DriveGridItemType = "file" | "folder";

/**
 * The grid delegates the actual upload to its owner. This keeps API concerns
 * (scope, progress reporting, cache invalidation, and refresh) out of a visual
 * component while still allowing a file card to offer "Upload new version".
 */
export type UploadNewVersionHandler = (
    existingFile: DriveFile,
    replacementFile: File,
    onProgress?: (percent: number) => void,
) => void | Promise<void>;

export interface FileGridProps {
    folders: DriveFolder[];
    files: DriveFile[];
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onRename?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onInfo?: (item: DriveGridItem, type: DriveGridItemType) => void;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelection: (item: DriveMoveItem) => void;
    onMoveItems?: (items: DriveMoveItem[], targetId: number) => void;
    onUploadNewVersion?: UploadNewVersionHandler;
    searchQuery?: string;
    /** Grid tiles have no column headers; sorting is driven from the DriveBar dropdown. */
    sortBy?: DriveSortKey;
    sortOrder?: DriveSortOrder;
    onSort?: (key: DriveSortKey) => void;
}

type FileIconConfig = {
    Icon: LucideIcon;
    className: string;
};

const FILE_ICON_BY_EXTENSION: Readonly<Record<string, FileIconConfig>> = {
    jpg: { Icon: FileImage, className: "file-grid-icon--image" },
    jpeg: { Icon: FileImage, className: "file-grid-icon--image" },
    png: { Icon: FileImage, className: "file-grid-icon--image" },
    gif: { Icon: FileImage, className: "file-grid-icon--image" },
    webp: { Icon: FileImage, className: "file-grid-icon--image" },
    svg: { Icon: FileImage, className: "file-grid-icon--image" },
    bmp: { Icon: FileImage, className: "file-grid-icon--image" },
    ico: { Icon: FileImage, className: "file-grid-icon--image" },

    js: { Icon: FileCode, className: "file-grid-icon--code" },
    ts: { Icon: FileCode, className: "file-grid-icon--code" },
    tsx: { Icon: FileCode, className: "file-grid-icon--code" },
    jsx: { Icon: FileCode, className: "file-grid-icon--code" },
    py: { Icon: FileCode, className: "file-grid-icon--code" },
    cpp: { Icon: FileCode, className: "file-grid-icon--code" },
    c: { Icon: FileCode, className: "file-grid-icon--code" },
    h: { Icon: FileCode, className: "file-grid-icon--code" },
    hpp: { Icon: FileCode, className: "file-grid-icon--code" },
    css: { Icon: FileCode, className: "file-grid-icon--code" },
    html: { Icon: FileCode, className: "file-grid-icon--code" },
    json: { Icon: FileCode, className: "file-grid-icon--code" },
    xml: { Icon: FileCode, className: "file-grid-icon--code" },
    yaml: { Icon: FileCode, className: "file-grid-icon--code" },
    yml: { Icon: FileCode, className: "file-grid-icon--code" },

    csv: { Icon: FileSpreadsheet, className: "file-grid-icon--sheet" },
    xls: { Icon: FileSpreadsheet, className: "file-grid-icon--sheet" },
    xlsx: { Icon: FileSpreadsheet, className: "file-grid-icon--sheet" },
    ods: { Icon: FileSpreadsheet, className: "file-grid-icon--sheet" },

    mp3: { Icon: Music, className: "file-grid-icon--audio" },
    wav: { Icon: Music, className: "file-grid-icon--audio" },
    ogg: { Icon: Music, className: "file-grid-icon--audio" },
    flac: { Icon: Music, className: "file-grid-icon--audio" },
    aac: { Icon: Music, className: "file-grid-icon--audio" },
    m4a: { Icon: Music, className: "file-grid-icon--audio" },

    mp4: { Icon: Video, className: "file-grid-icon--video" },
    avi: { Icon: Video, className: "file-grid-icon--video" },
    mkv: { Icon: Video, className: "file-grid-icon--video" },
    mov: { Icon: Video, className: "file-grid-icon--video" },
    wmv: { Icon: Video, className: "file-grid-icon--video" },
    webm: { Icon: Video, className: "file-grid-icon--video" },

    zip: { Icon: Archive, className: "file-grid-icon--archive" },
    rar: { Icon: Archive, className: "file-grid-icon--archive" },
    "7z": { Icon: Archive, className: "file-grid-icon--archive" },
    tar: { Icon: Archive, className: "file-grid-icon--archive" },
    gz: { Icon: Archive, className: "file-grid-icon--archive" },
    bz2: { Icon: Archive, className: "file-grid-icon--archive" },
    xz: { Icon: Archive, className: "file-grid-icon--archive" },
    zst: { Icon: Archive, className: "file-grid-icon--archive" },
    pdf: { Icon: FileText, className: "file-grid-icon--pdf" },
};

const DEFAULT_FILE_ICON: FileIconConfig = {
    Icon: FileIcon,
    className: "file-grid-icon--generic",
};

function formatFileSize(bytes: number): string {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";

    const units = ["B", "KB", "MB", "GB", "TB"] as const;
    const unitIndex = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
    const value = bytes / 1024 ** unitIndex;

    return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

function getFileIcon(extension: string): React.ReactElement {
    const normalizedExtension = extension.toLowerCase().replace(/^\./, "");
    const { Icon, className } = FILE_ICON_BY_EXTENSION[normalizedExtension] ?? DEFAULT_FILE_ICON;

    return <Icon size={20} className={`file-grid-icon ${className}`} />;
}

function getFolderLabel(folder: DriveFolder): string {
    return folder.display_name || folder.name;
}

function toFolderMoveItem(folder: DriveFolder): DriveMoveItem | null {
    if (folder.id == null) return null;

    return {
        id: folder.id,
        type: "folder",
        source_path: folder.path,
        name: getFolderLabel(folder),
    };
}

function toFileMoveItem(file: DriveFile): DriveMoveItem {
    return {
        id: file.id,
        type: "file",
        source_path: file.path,
        name: file.name,
    };
}

/** Validate data read from DataTransfer before treating it as an internal item. */
function isDriveMoveItem(value: unknown): value is DriveMoveItem {
    if (typeof value !== "object" || value === null) return false;

    const candidate = value as Record<string, unknown>;
    return (
        (candidate.type === "file" || candidate.type === "folder") &&
        typeof candidate.id === "number" &&
        Number.isFinite(candidate.id) &&
        typeof candidate.source_path === "string" &&
        typeof candidate.name === "string"
    );
}

function handleDragStart(event: React.DragEvent<HTMLElement>, item: DriveMoveItem): void {
    event.dataTransfer.setData("application/json", JSON.stringify(item));
    event.dataTransfer.effectAllowed = "move";
}

function FileGridEmptyState(): React.ReactElement {
    return (
        <div className="drive-empty">
            <FileIcon size={48} strokeWidth={1} className="drive-empty-icon" />
            <p className="drive-empty-label">Directory is empty</p>
        </div>
    );
}

interface CardActionButtonProps {
    icon: LucideIcon;
    label: string;
    onClick: () => void;
    danger?: boolean;
}

function CardActionButton({ icon: Icon, label, onClick, danger = false }: CardActionButtonProps): React.ReactElement {
    return (
        <button
            type="button"
            className={`file-card-action-btn${danger ? " file-card-action-btn--danger" : ""}`}
            title={label}
            aria-label={label}
            onClick={(event) => {
                event.stopPropagation();
                onClick();
            }}
        >
            <Icon size={11} />
        </button>
    );
}

interface FolderCardProps {
    folder: DriveFolder;
    selected: boolean;
    dragOver: boolean;
    searchQuery: string;
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onInfo?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onRename?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onDelete?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onToggleSelection: (item: DriveMoveItem) => void;
    onMoveItems?: (items: DriveMoveItem[], targetId: number) => void;
    onDrop: (event: React.DragEvent<HTMLDivElement>, folder: DriveFolder) => void;
    setDragOverPath: React.Dispatch<React.SetStateAction<DriveFolder["path"] | null>>;
}

function FolderCard({
    folder,
    selected,
    dragOver,
    searchQuery,
    onFolderClick,
    onFolderDownload,
    onInfo,
    onRename,
    onDelete,
    onToggleSelection,
    onMoveItems,
    onDrop,
    setDragOverPath,
}: FolderCardProps): React.ReactElement {
    const moveItem = toFolderMoveItem(folder);
    const canMove = moveItem !== null && onMoveItems !== undefined;

    return (
        <div
            className={`file-card group${selected ? " file-card--selected" : ""}${dragOver ? " file-card--drag-over" : ""}`}
            draggable={canMove}
            onClick={() => onFolderClick(folder)}
            onDragStart={(event) => {
                if (moveItem && canMove) handleDragStart(event, moveItem);
            }}
            onDragOver={(event) => {
                if (!onMoveItems || folder.id == null) return;
                event.preventDefault();
                event.dataTransfer.dropEffect = "move";
                setDragOverPath(folder.path);
            }}
            onDragLeave={(event) => {
                const nextTarget = event.relatedTarget;
                if (nextTarget instanceof Node && event.currentTarget.contains(nextTarget)) {
                    return;
                }
                setDragOverPath((currentPath) => (currentPath === folder.path ? null : currentPath));
            }}
            onDragEnd={() => setDragOverPath(null)}
            onDrop={(event) => onDrop(event, folder)}
        >
            <div className="file-card-top">
                <div className="file-card-icon-wrap">
                    <Folder
                        className={folder.token ? "file-grid-icon file-grid-icon--service" : "file-grid-icon file-grid-icon--folder"}
                        size={20}
                    />
                </div>
                {moveItem && (
                    <div className="file-card-check">
                        <input
                            type="checkbox"
                            checked={selected}
                            aria-label={`Select folder ${getFolderLabel(folder)}`}
                            className="checkbox-industrial"
                            onChange={() => onToggleSelection(moveItem)}
                            onClick={(event) => event.stopPropagation()}
                        />
                    </div>
                )}
            </div>

            <div className="file-card-body">
                <div className="file-card-name">
                    <Highlight text={getFolderLabel(folder)} query={searchQuery} />
                </div>
                <div className="file-card-type">{folder.token ? "INFRASTRUCTURE" : "DIRECTORY"}</div>
            </div>

            <div className="file-card-actions" onClick={(event) => event.stopPropagation()}>
                {folder.id != null && onFolderDownload && (
                    <CardActionButton icon={Download} label="Download folder" onClick={() => onFolderDownload(folder)} />
                )}
                {folder.id != null && onInfo && (
                    <CardActionButton icon={Info} label="Folder information" onClick={() => onInfo(folder, "folder")} />
                )}
                {folder.id != null && onRename && (
                    <CardActionButton icon={Edit3} label="Rename folder" onClick={() => onRename(folder, "folder")} />
                )}
                {folder.id != null && onDelete && (
                    <CardActionButton icon={Trash2} label="Delete folder" danger onClick={() => onDelete(folder, "folder")} />
                )}
            </div>
        </div>
    );
}

interface UploadVersionButtonProps {
    file: DriveFile;
    onUploadNewVersion: UploadNewVersionHandler;
}

type UploadStatus = "idle" | "uploading" | "success" | "error";

function UploadVersionButton({ file, onUploadNewVersion }: UploadVersionButtonProps): React.ReactElement {
    const inputRef = useRef<HTMLInputElement>(null);
    const [status, setStatus] = useState<UploadStatus>("idle");
    const [errorMessage, setErrorMessage] = useState<string | null>(null);

    const handleFileChange = async (event: React.ChangeEvent<HTMLInputElement>): Promise<void> => {
        const replacementFile = event.currentTarget.files?.[0];
        // Reset so selecting the same local file again triggers onChange.
        event.currentTarget.value = "";
        if (!replacementFile) return;

        setStatus("uploading");
        setErrorMessage(null);

        try {
            await onUploadNewVersion(file, replacementFile);
            setStatus("success");
        } catch (error: unknown) {
            setStatus("error");
            setErrorMessage(error instanceof Error ? error.message : "Upload failed");
        }
    };

    return (
        <>
            <input
                ref={inputRef}
                type="file"
                className="visually-hidden"
                tabIndex={-1}
                aria-label={`Choose a replacement file for ${file.name}`}
                onChange={handleFileChange}
            />
            <button
                type="button"
                className="file-card-action-btn"
                title={status === "uploading" ? "Uploading new version…" : "Upload new version"}
                aria-label="Upload new version"
                aria-busy={status === "uploading"}
                disabled={status === "uploading"}
                onClick={(event) => {
                    event.stopPropagation();
                    inputRef.current?.click();
                }}
            >
                {status === "uploading" ? <Loader2 size={11} className="drive-spin" /> : <History size={11} />}
            </button>
            <span className="visually-hidden" role="status" aria-live="polite">
                {status === "uploading" && "Uploading new version"}
                {status === "success" && "New version uploaded"}
                {status === "error" && errorMessage}
            </span>
        </>
    );
}

interface FileDownloadActionProps {
    file: DriveFile;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
}

function FileDownloadAction({ file, onFileClick, onDownloadDecompressed }: FileDownloadActionProps): React.ReactElement | null {
    const isZstdFile = file.name.toLowerCase().endsWith(".zst");
    if (!isZstdFile || !onDownloadDecompressed) return null;

    return (
        <div onClick={(event) => event.stopPropagation()}>
            <DropdownMenu>
                <DropdownMenuTrigger asChild>
                    <button type="button" className="file-card-action-btn" title="Download options" aria-label="Download options">
                        <Download size={11} />
                    </button>
                </DropdownMenuTrigger>
                <DropdownMenuContent align="end" className="w-48 bg-popover border-border text-popover-foreground shadow-xl">
                    <DropdownMenuItem
                        className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                        onClick={() => onDownloadDecompressed(file)}
                    >
                        Download Decompressed
                    </DropdownMenuItem>
                    <DropdownMenuItem className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2" onClick={() => onFileClick(file)}>
                        Download Original (.zst)
                    </DropdownMenuItem>
                </DropdownMenuContent>
            </DropdownMenu>
        </div>
    );
}

interface FileCardProps {
    file: DriveFile;
    selected: boolean;
    searchQuery: string;
    onFileClick: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onInfo?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onRename?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onDelete?: (item: DriveGridItem, type: DriveGridItemType) => void;
    onToggleSelection: (item: DriveMoveItem) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onUploadNewVersion?: UploadNewVersionHandler;
    onDragStart: (event: React.DragEvent<HTMLDivElement>, file: DriveFile) => void;
}

function FileCard({
    file,
    selected,
    searchQuery,
    onFileClick,
    onPreview,
    onInfo,
    onRename,
    onDelete,
    onToggleSelection,
    onDownloadDecompressed,
    onUploadNewVersion,
    onDragStart,
}: FileCardProps): React.ReactElement {
    const moveItem = toFileMoveItem(file);

    return (
        <div
            className={`file-card group${selected ? " file-card--selected" : ""}`}
            draggable
            onDragStart={(event) => onDragStart(event, file)}
            onClick={() => onFileClick(file)}
        >
            <div className="file-card-top">
                <div className="file-card-icon-wrap">{getFileIcon(file.extension)}</div>
                <div className="file-card-check">
                    <input
                        type="checkbox"
                        checked={selected}
                        aria-label={`Select file ${file.name}`}
                        className="checkbox-industrial"
                        onChange={() => onToggleSelection(moveItem)}
                        onClick={(event) => event.stopPropagation()}
                    />
                </div>
            </div>

            <div className="file-card-body">
                <div className="file-card-name">
                    <Highlight text={file.name} query={searchQuery} />
                </div>
                <div className="file-card-meta">
                    <span className="file-card-type">{file.extension || "DATA"}</span>
                    <span className="file-card-size">{formatFileSize(file.size)}</span>
                    {file.is_encrypted && (
                        <span
                            className="inline-flex items-center gap-0.5 text-[9px] font-mono text-amber-500 font-semibold"
                            title={`Encrypted (Key: ${file.key_id || "AES-256-GCM"})`}
                        >
                            <Lock size={10} className="stroke-[2.5]" />
                            <span>ENC</span>
                        </span>
                    )}
                </div>
            </div>

            <div className="file-card-actions" onClick={(event) => event.stopPropagation()}>
                {onPreview && <CardActionButton icon={Eye} label="Preview file" onClick={() => onPreview(file)} />}
                {onInfo && <CardActionButton icon={Info} label="File information" onClick={() => onInfo(file, "file")} />}
                {onRename && <CardActionButton icon={Edit3} label="Rename file" onClick={() => onRename(file, "file")} />}
                {onUploadNewVersion && <UploadVersionButton file={file} onUploadNewVersion={onUploadNewVersion} />}
                <FileDownloadAction file={file} onFileClick={onFileClick} onDownloadDecompressed={onDownloadDecompressed} />
                {onDelete && <CardActionButton icon={Trash2} label="Delete file" danger onClick={() => onDelete(file, "file")} />}
            </div>
        </div>
    );
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
    onUploadNewVersion,
    searchQuery = "",
}: FileGridProps): React.ReactElement {
    const [dragOverPath, setDragOverPath] = useState<DriveFolder["path"] | null>(null);

    const handleDrop = (event: React.DragEvent<HTMLDivElement>, targetFolder: DriveFolder): void => {
        event.preventDefault();
        setDragOverPath(null);

        if (!onMoveItems || targetFolder.id == null) return;

        const serializedItem = event.dataTransfer.getData("application/json");
        if (!serializedItem) return;

        let draggedItem: unknown;
        try {
            draggedItem = JSON.parse(serializedItem) as unknown;
        } catch {
            return;
        }

        if (!isDriveMoveItem(draggedItem)) return;
        if (draggedItem.type === "folder" && draggedItem.id === targetFolder.id) {
            return;
        }

        onMoveItems([draggedItem], targetFolder.id);
    };

    const hasItems = folders.length > 0 || files.length > 0;
    if (!hasItems) {
        return <FileGridEmptyState />;
    }

    return (
        <div className="grid-industrial-drive">
            {folders.map((folder) => {
                const moveItem = toFolderMoveItem(folder);
                const selected = moveItem ? isSelected(moveItem) : false;

                return (
                    <FolderCard
                        key={folder.path}
                        folder={folder}
                        selected={selected}
                        dragOver={dragOverPath === folder.path}
                        searchQuery={searchQuery}
                        onFolderClick={onFolderClick}
                        onFolderDownload={onFolderDownload}
                        onInfo={onInfo}
                        onRename={onRename}
                        onDelete={onDelete}
                        onToggleSelection={onToggleSelection}
                        onMoveItems={onMoveItems}
                        onDrop={handleDrop}
                        setDragOverPath={setDragOverPath}
                    />
                );
            })}

            {files.map((file) => {
                const moveItem = toFileMoveItem(file);
                return (
                    <FileCard
                        key={file.id}
                        file={file}
                        selected={isSelected(moveItem)}
                        searchQuery={searchQuery}
                        onFileClick={onFileClick}
                        onPreview={onPreview}
                        onInfo={onInfo}
                        onRename={onRename}
                        onDelete={onDelete}
                        onToggleSelection={onToggleSelection}
                        onDownloadDecompressed={onDownloadDecompressed}
                        onUploadNewVersion={onUploadNewVersion}
                        onDragStart={(event, draggedFile) => {
                            handleDragStart(event, toFileMoveItem(draggedFile));
                        }}
                    />
                );
            })}
        </div>
    );
}
