// @/pages/drive/components/DetailsModal.tsx
//
// Centered dialog with metadata for the selected file or folder.
//
// Layout of this file:
//   DetailsModal   – overlay, dialog chrome, Escape / backdrop-click closing
//   DetailsHeading – icon, name and kind
//   DetailList     – rows built from a plain { icon, label, value } array
//   FolderDetails / FileDetails – build the rows for each item type

import { useEffect, useRef, type ReactNode } from "react";
import { X, File, Folder, Calendar, HardDrive, Info, Lock } from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import { toDisplayDrivePath } from "@/pages/drive/pathDisplay";
import { formatDate, formatFileSize } from "@/pages/drive/format";
import { VersionHistory } from "@/pages/drive/components/VersionHistory";
import type { StorageScope } from "@/pages/drive/api";

type DetailsItem = DriveFolder | DriveFile;

interface DetailsModalProps {
    item: DetailsItem | null;
    onClose: () => void;
    scope?: StorageScope;
    onChanged?: () => void;
    canWrite?: boolean;
    canDelete?: boolean;
}

interface DetailRowData {
    icon: ReactNode;
    label: string;
    value: string;
    wrap?: boolean;
}

function isDriveFolder(item: DetailsItem): item is DriveFolder {
    return "count_sub_files" in item;
}

function describeEncryption(file: DriveFile): string {
    if (!file.is_encrypted) return "Plaintext";
    const keyHint = file.key_id ? ` · ${file.key_id.slice(0, 8)}...` : "";
    return `AES-256-GCM (Protected${keyHint})`;
}

function describeStorage(file: DriveFile): string {
    return file.upload_mode === "multipart" ? `Multipart · ${file.part_count ?? "?"} parts` : "Single PUT";
}

// ─── pieces ──────────────────────────────────────────────────────────────────

function DetailsHeading({ name, kind, isFolder }: { name: string; kind: string; isFolder: boolean }) {
    return (
        <div className="drive-details-heading">
            <div className="drive-details-heading-icon">{isFolder ? <Folder size={40} /> : <File size={40} />}</div>
            <h3 className="drive-details-heading-name">{name}</h3>
            <span className="drive-details-heading-kind">{kind}</span>
        </div>
    );
}

function DetailList({ rows }: { rows: DetailRowData[] }) {
    return (
        <div className="drive-details-list">
            {rows.map((row) => (
                <div key={row.label} className="drive-detail-item">
                    <div className="drive-detail-item-icon">{row.icon}</div>
                    <div className="drive-detail-item-text">
                        <span className="drive-detail-item-label">{row.label}</span>
                        <span className={`drive-detail-item-value${row.wrap ? " drive-detail-item-value--wrap" : ""}`}>{row.value}</span>
                    </div>
                </div>
            ))}
        </div>
    );
}

function FolderDetails({ folder }: { folder: DriveFolder }) {
    const rows: DetailRowData[] = [
        { icon: <HardDrive size={16} />, label: "Size", value: formatFileSize(folder.virtual_size ?? 0) },
        { icon: <Calendar size={16} />, label: "Created At", value: formatDate(folder.created_at) },
        { icon: <Info size={16} />, label: "Path", value: toDisplayDrivePath(folder.path), wrap: true },
        { icon: <File size={14} />, label: "Sub-files", value: String(folder.count_sub_files ?? 0) },
        { icon: <Folder size={14} />, label: "Sub-folders", value: String(folder.count_sub_directories ?? 0) },
    ];

    return (
        <>
            <DetailsHeading name={folder.name} kind="Folder" isFolder />
            <DetailList rows={rows} />
        </>
    );
}

interface FileDetailsProps {
    file: DriveFile;
    scope: StorageScope;
    canWrite: boolean;
    canDelete: boolean;
    onChanged?: () => void;
}

function FileDetails({ file, scope, canWrite, canDelete, onChanged }: FileDetailsProps) {
    const rows: DetailRowData[] = [
        { icon: <HardDrive size={16} />, label: "Size", value: formatFileSize(file.size ?? 0) },
        ...(file.original_size !== file.size
            ? [{ icon: <HardDrive size={16} />, label: "Uncompressed Size", value: formatFileSize(file.original_size ?? 0) }]
            : []),
        { icon: <HardDrive size={16} />, label: "Storage", value: describeStorage(file) },
        { icon: <Lock size={16} />, label: "Encryption", value: describeEncryption(file) },
        { icon: <Calendar size={16} />, label: "Created At", value: formatDate(file.created_at) },
        { icon: <Info size={16} />, label: "Path", value: toDisplayDrivePath(file.path), wrap: true },
    ];

    return (
        <>
            <DetailsHeading name={file.name} kind={file.extension ? `.${file.extension} file` : "File"} isFolder={false} />
            <DetailList rows={rows} />
            <VersionHistory file={file} scope={scope} canWrite={canWrite} canDelete={canDelete} onChanged={onChanged} />
        </>
    );
}

// ─── dialog ──────────────────────────────────────────────────────────────────

export function DetailsModal({ item, onClose, scope = "personal", onChanged, canWrite = true, canDelete = true }: DetailsModalProps) {
    const dialogRef = useRef<HTMLDivElement>(null);
    const isOpen = item !== null;

    // Close on Escape and move focus into the dialog while it is open.
    useEffect(() => {
        if (!isOpen) return;
        dialogRef.current?.focus();

        const handleKeyDown = (event: KeyboardEvent) => {
            if (event.key === "Escape") onClose();
        };
        document.addEventListener("keydown", handleKeyDown);
        return () => document.removeEventListener("keydown", handleKeyDown);
    }, [isOpen, onClose]);

    if (!item) return null;

    return (
        <div className="drive-details-overlay" onClick={onClose}>
            <div
                ref={dialogRef}
                role="dialog"
                aria-modal="true"
                aria-label="Selected item details"
                tabIndex={-1}
                className="drive-details-modal"
                onClick={(event) => event.stopPropagation()}
            >
                <div className="drive-details-header">
                    <div className="drive-details-title">
                        <Info size={18} className="text-primary" />
                        <span>Details</span>
                    </div>
                    <button type="button" onClick={onClose} aria-label="Close details" className="drive-details-close">
                        <X size={20} />
                    </button>
                </div>
                <div className="drive-details-body">
                    {isDriveFolder(item) ? (
                        <FolderDetails folder={item} />
                    ) : (
                        <FileDetails file={item} scope={scope} canWrite={canWrite} canDelete={canDelete} onChanged={onChanged} />
                    )}
                </div>
            </div>
        </div>
    );
}
