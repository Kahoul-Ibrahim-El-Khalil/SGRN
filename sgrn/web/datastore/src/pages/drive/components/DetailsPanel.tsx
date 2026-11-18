import React from "react";
import { X, File, Folder, Calendar, HardDrive, Info, Lock } from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import { toDisplayDrivePath } from "@/pages/drive/pathDisplay";
import { VersionHistory } from "@/pages/drive/components/VersionHistory";
import type { StorageScope } from "@/pages/drive/api";

type DetailsItem = DriveFolder | DriveFile;

interface DetailsPanelProps {
    item: DetailsItem | null;
    onClose: () => void;
    scope?: StorageScope;
    onChanged?: () => void;
    canWrite?: boolean;
    canDelete?: boolean;
}

const SIZE_UNITS = ["Bytes", "KB", "MB", "GB", "TB"] as const;

function formatSize(bytes: number): string {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 Bytes";
    let value = bytes;
    let unitIndex = 0;
    while (value >= 1024 && unitIndex < SIZE_UNITS.length - 1) {
        value /= 1024;
        unitIndex += 1;
    }
    return `${Number(value.toFixed(2))} ${SIZE_UNITS[unitIndex]}`;
}

function isDriveFolder(item: DetailsItem): item is DriveFolder {
    return "count_sub_files" in item;
}

export const DetailsPanel: React.FC<DetailsPanelProps> = ({
    item,
    onClose,
    scope = "personal",
    onChanged,
    canWrite = true,
    canDelete = true,
}) => {
    if (!item) return null;

    return (
        <aside aria-label="Selected item details" className="drive-details-panel">
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
        </aside>
    );
};

function DetailsHeading({ name, kind }: { name: string; kind: string }) {
    return (
        <div className="drive-details-heading">
            <div className="drive-details-heading-icon">{kind === "Folder" ? <Folder size={40} /> : <File size={40} />}</div>
            <h3 className="drive-details-heading-name">{name}</h3>
            <span className="drive-details-heading-kind">{kind}</span>
        </div>
    );
}

function FolderDetails({ folder }: { folder: DriveFolder }) {
    return (
        <>
            <DetailsHeading name={folder.name} kind="Folder" />
            <div className="drive-details-list">
                <DetailItem icon={<HardDrive size={16} />} label="Size" value={formatSize(folder.virtual_size ?? 0)} />
                <DetailItem icon={<Calendar size={16} />} label="Created At" value={formatDate(folder.created_at)} />
                <DetailItem icon={<Info size={16} />} label="Path" value={toDisplayDrivePath(folder.path)} wrap />
                <DetailItem icon={<File size={14} />} label="Sub-files" value={String(folder.count_sub_files ?? 0)} />
                <DetailItem icon={<Folder size={14} />} label="Sub-folders" value={String(folder.count_sub_directories ?? 0)} />
            </div>
        </>
    );
}

function FileDetails({
    file,
    scope,
    canWrite,
    canDelete,
    onChanged,
}: {
    file: DriveFile;
    scope: StorageScope;
    canWrite: boolean;
    canDelete: boolean;
    onChanged?: () => void;
}) {
    return (
        <>
            <DetailsHeading name={file.name} kind={file.extension ? `.${file.extension} file` : "File"} />
            <div className="drive-details-list">
                <DetailItem icon={<HardDrive size={16} />} label="Size" value={formatSize(file.size ?? 0)} />
                {file.original_size !== file.size && (
                    <DetailItem icon={<HardDrive size={16} />} label="Uncompressed Size" value={formatSize(file.original_size ?? 0)} />
                )}
                <DetailItem
                    icon={<HardDrive size={16} />}
                    label="Storage"
                    value={file.upload_mode === "multipart" ? `Multipart · ${file.part_count ?? "?"} parts` : "Single PUT"}
                />
                <DetailItem
                    icon={<Lock size={16} />}
                    label="Encryption"
                    value={
                        file.is_encrypted ? `AES-256-GCM (Protected${file.key_id ? ` · ${file.key_id.slice(0, 8)}...` : ""})` : "Plaintext"
                    }
                />
                <DetailItem icon={<Calendar size={16} />} label="Created At" value={formatDate(file.created_at)} />
                <DetailItem icon={<Info size={16} />} label="Path" value={toDisplayDrivePath(file.path)} wrap />
            </div>
            <VersionHistory file={file} scope={scope} canWrite={canWrite} canDelete={canDelete} onChanged={onChanged} />
        </>
    );
}

function formatDate(value: string | null | undefined): string {
    if (!value) return "Unknown";
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? "Unknown" : date.toLocaleString();
}

function DetailItem({ icon, label, value, wrap = false }: { icon: React.ReactNode; label: string; value: string; wrap?: boolean }) {
    return (
        <div className="drive-detail-item">
            <div className="drive-detail-item-icon">{icon}</div>
            <div className="drive-detail-item-text">
                <span className="drive-detail-item-label">{label}</span>
                <span className={`drive-detail-item-value${wrap ? " drive-detail-item-value--wrap" : ""}`}>{value}</span>
            </div>
        </div>
    );
}
