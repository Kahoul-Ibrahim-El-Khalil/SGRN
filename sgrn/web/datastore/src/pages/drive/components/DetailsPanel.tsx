import React from "react";
import { X, File, Folder, Calendar, HardDrive, User, Info, Lock } from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import { toDisplayDrivePath } from "@/pages/drive/pathDisplay";

interface DetailsPanelProps {
    item: DriveFolder | DriveFile | null;
    onClose: () => void;
}

// Module-level constants: initialized once.
const SIZE_UNITS: readonly string[] = ["Bytes", "KB", "MB", "GB", "TB"];

const SIZE_BASE = 1024;

// Module-level formatter: reused across renders.
const formatSize = (bytes: number): string => {
    if (bytes <= 0) return "0 Bytes";

    let value = bytes;
    let unitIndex = 0;

    while (value >= SIZE_BASE && unitIndex < SIZE_UNITS.length - 1) {
        value /= SIZE_BASE;
        unitIndex++;
    }

    return `${parseFloat(value.toFixed(2))} ${SIZE_UNITS[unitIndex]}`;
};

export const DetailsPanel: React.FC<DetailsPanelProps> = ({ item, onClose }) => {
    if (!item) return null;

    const isFolder = "count_sub_files" in item;

    return (
        <div
            className="drive-details-panel"
            style={{
                width: "300px",
                backgroundColor: "hsl(var(--card) / 0.95)",
                borderLeft: "1px solid hsl(var(--border))",
                display: "flex",
                flexDirection: "column",
                height: "100%",
                position: "absolute",
                right: 0,
                top: 0,
                zIndex: 100,
                backdropFilter: "blur(10px)",
                boxShadow: "var(--shadow-lg)",
                color: "hsl(var(--foreground))",
            }}
        >
            <div
                style={{
                    padding: "1rem",
                    display: "flex",
                    alignItems: "center",
                    justifyContent: "space-between",
                    borderBottom: "1px solid hsl(var(--border))",
                }}
            >
                <div style={{ display: "flex", alignItems: "center", gap: "0.5rem", fontWeight: 600 }}>
                    <Info size={18} className="text-primary" />
                    <span>Details</span>
                </div>
                <button
                    onClick={onClose}
                    style={{
                        background: "none",
                        border: "none",
                        color: "hsl(var(--muted-foreground))",
                        cursor: "pointer",
                        padding: "0.25rem",
                    }}
                >
                    <X size={20} />
                </button>
            </div>
            <div style={{ padding: "1.5rem", overflowY: "auto", flex: 1 }}>
                <div
                    style={{
                        display: "flex",
                        flexDirection: "column",
                        alignItems: "center",
                        textAlign: "center",
                        marginBottom: "2rem",
                    }}
                >
                    <div
                        style={{
                            width: "80px",
                            height: "80px",
                            borderRadius: "0", // Enforce industrial hard corners
                            backgroundColor: "hsl(var(--primary) / 0.1)",
                            display: "flex",
                            alignItems: "center",
                            justifyContent: "center",
                            marginBottom: "1rem",
                            color: "hsl(var(--primary))",
                        }}
                    >
                        {isFolder ? <Folder size={40} /> : <File size={40} />}
                    </div>
                    <h3 style={{ fontSize: "1.1rem", marginBottom: "0.25rem", wordBreak: "break-all", color: "hsl(var(--foreground))" }}>
                        {item.name}
                    </h3>
                    <span style={{ fontSize: "0.8rem", color: "hsl(var(--muted-foreground))" }}>
                        {isFolder ? "Folder" : (item as DriveFile).extension ? `.${(item as DriveFile).extension} file` : "File"}
                    </span>
                </div>
                <div style={{ display: "flex", flexDirection: "column", gap: "1.25rem" }}>
                    <DetailItem
                        icon={<HardDrive size={16} />}
                        label="Size"
                        value={isFolder ? formatSize((item as DriveFolder).virtual_size || 0) : formatSize((item as DriveFile).size || 0)}
                    />
                    {!isFolder && (item as DriveFile).original_size !== (item as DriveFile).size && (
                        <DetailItem
                            icon={<HardDrive size={16} />}
                            label="Uncompressed Size"
                            value={formatSize((item as DriveFile).original_size || 0)}
                        />
                    )}
                    {!isFolder && (
                        <DetailItem
                            icon={<HardDrive size={16} />}
                            label="Storage"
                            value={
                                (item as DriveFile).upload_mode === "multipart"
                                    ? `Multipart · ${(item as DriveFile).part_count ?? "?"} parts`
                                    : "Single PUT"
                            }
                        />
                    )}
                    {!isFolder && (
                        <DetailItem
                            icon={<Lock size={16} />}
                            label="Encryption"
                            value={
                                (item as DriveFile).is_encrypted
                                    ? `AES-256-GCM (Protected${(item as DriveFile).key_id ? ` · ${(item as DriveFile).key_id?.slice(0, 8)}...` : ""})`
                                    : "Plaintext"
                            }
                        />
                    )}
                    <DetailItem
                        icon={<Calendar size={16} />}
                        label="Created At"
                        value={item.created_at ? new Date(item.created_at).toLocaleString() : "Unknown"}
                    />
                    <DetailItem icon={<User size={16} />} label="Owner" value="Personal" />
                    <DetailItem
                        icon={<Info size={16} />}
                        label="Path"
                        value={toDisplayDrivePath(item.path)}
                        style={{ wordBreak: "break-all" }}
                    />
                    {isFolder && (
                        <>
                            <DetailItem
                                icon={<File size={14} />}
                                label="Sub-files"
                                value={((item as DriveFolder).count_sub_files ?? 0).toString()}
                            />
                            <DetailItem
                                icon={<Folder size={14} />}
                                label="Sub-folders"
                                value={((item as DriveFolder).count_sub_directories ?? 0).toString()}
                            />
                        </>
                    )}
                </div>
            </div>
        </div>
    );
};

const DetailItem: React.FC<{ icon: React.ReactNode; label: string; value: string; style?: React.CSSProperties }> = ({
    icon,
    label,
    value,
    style,
}) => (
    <div style={{ display: "flex", gap: "0.75rem", alignItems: "flex-start" }}>
        <div style={{ color: "hsl(var(--muted-foreground) / 0.5)", marginTop: "0.15rem" }}>{icon}</div>
        <div style={{ display: "flex", flexDirection: "column", gap: "0.1rem" }}>
            <span
                style={{ fontSize: "0.75rem", color: "hsl(var(--muted-foreground))", textTransform: "uppercase", letterSpacing: "0.05em" }}
            >
                {label}
            </span>
            <span style={{ fontSize: "0.9rem", color: "hsl(var(--foreground))", ...style }}>{value}</span>
        </div>
    </div>
);
