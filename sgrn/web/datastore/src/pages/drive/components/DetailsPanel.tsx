import React from "react";
import { X, File, Folder, Calendar, HardDrive, User, Info } from "lucide-react";
import type { DriveFolder, DriveFile } from "@sgrn/types";

interface DetailsPanelProps {
    item: DriveFolder | DriveFile | null;
    onClose: () => void;
}

export const DetailsPanel: React.FC<DetailsPanelProps> = ({ item, onClose }) => {
    if (!item) return null;

    const isFolder = "count_sub_files" in item;

    const formatSize = (bytes: number) => {
        if (bytes === 0) return "0 Bytes";
        const k = 1024;
        const sizes = ["Bytes", "KB", "MB", "GB", "TB"];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + " " + sizes[i];
    };

    return (
        <div
            className="drive-details-panel"
            style={{
                width: "300px",
                backgroundColor: "rgba(15, 15, 20, 0.95)",
                borderLeft: "1px solid rgba(255, 255, 255, 0.1)",
                display: "flex",
                flexDirection: "column",
                height: "100%",
                position: "absolute",
                right: 0,
                top: 0,
                zIndex: 100,
                backdropFilter: "blur(10px)",
                boxShadow: "-10px 0 30px rgba(0, 0, 0, 0.5)",
            }}
        >
            <div
                style={{
                    padding: "1rem",
                    display: "flex",
                    alignItems: "center",
                    justifyContent: "space-between",
                    borderBottom: "1px solid rgba(255, 255, 255, 0.05)",
                }}
            >
                <div style={{ display: "flex", alignItems: "center", gap: "0.5rem", fontWeight: 600 }}>
                    <Info size={18} className="text-blue-400" />
                    <span>Details</span>
                </div>
                <button
                    onClick={onClose}
                    style={{
                        background: "none",
                        border: "none",
                        color: "rgba(255, 255, 255, 0.4)",
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
                            borderRadius: "1rem",
                            backgroundColor: isFolder ? "rgba(59, 130, 246, 0.1)" : "rgba(16, 185, 129, 0.1)",
                            display: "flex",
                            alignItems: "center",
                            justifyContent: "center",
                            marginBottom: "1rem",
                            color: isFolder ? "#3b82f6" : "#10b981",
                        }}
                    >
                        {isFolder ? <Folder size={40} /> : <File size={40} />}
                    </div>
                    <h3 style={{ fontSize: "1.1rem", marginBottom: "0.25rem", wordBreak: "break-all" }}>{item.name}</h3>
                    <span style={{ fontSize: "0.8rem", color: "rgba(255, 255, 255, 0.4)" }}>
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
                    <DetailItem
                        icon={<Calendar size={16} />}
                        label="Created At"
                        value={item.created_at ? new Date(item.created_at).toLocaleString() : "Unknown"}
                    />
                    <DetailItem icon={<User size={16} />} label="Owner" value="Personal" />
                    <DetailItem icon={<Info size={16} />} label="Path" value={item.path} style={{ wordBreak: "break-all" }} />
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
        <div style={{ color: "rgba(255, 255, 255, 0.2)", marginTop: "0.15rem" }}>{icon}</div>
        <div style={{ display: "flex", flexDirection: "column", gap: "0.1rem" }}>
            <span style={{ fontSize: "0.75rem", color: "rgba(255, 255, 255, 0.4)", textTransform: "uppercase", letterSpacing: "0.05em" }}>
                {label}
            </span>
            <span style={{ fontSize: "0.9rem", color: "rgba(255, 255, 255, 0.9)", ...style }}>{value}</span>
        </div>
    </div>
);
