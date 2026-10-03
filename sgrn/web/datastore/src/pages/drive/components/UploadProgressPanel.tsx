import React from "react";
import { Upload, Download, CheckCircle2, AlertCircle, X, Pause, Play, ChevronDown, ChevronUp, FileText } from "lucide-react";

export interface UploadProgressItem {
    id: string;
    fileName: string;
    fileSize: number;
    status: "uploading" | "paused" | "completed" | "error";
    percent: number;
    uploadedChunks?: number;
    totalChunks?: number;
    errorMessage?: string;
    uploadedBytes?: number;
    direction?: "upload" | "download";
    uploadId?: string;
}

interface UploadProgressPanelProps {
    items: UploadProgressItem[];
    onCancel?: (id: string) => void;
    onPauseToggle?: (id: string) => void;
    onClearCompleted?: () => void;
}

export const UploadProgressPanel: React.FC<UploadProgressPanelProps> = ({ items, onCancel, onPauseToggle, onClearCompleted }) => {
    const [collapsed, setCollapsed] = React.useState(false);

    if (items.length === 0) return null;

    const activeCount = items.filter((i) => i.status === "uploading" || i.status === "paused").length;
    const completedCount = items.filter((i) => i.status === "completed").length;
    const hasDownloads = items.some((i) => i.direction === "download");
    const hasUploads = items.some((i) => i.direction !== "download");

    const formatSize = (bytes: number) => {
        if (!bytes || bytes === 0) return "0 B";
        const k = 1024;
        const sizes = ["B", "KB", "MB", "GB"];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + " " + sizes[i];
    };

    const titleText = hasUploads && hasDownloads ? "Transfers" : hasDownloads ? "Downloads" : "Uploads";

    return (
        <div className="transfer-panel-root">
            {/* Header */}
            <div className="transfer-panel-header">
                <div className="transfer-panel-title">
                    {hasDownloads && !hasUploads ? (
                        <Download className={activeCount > 0 ? "transfer-panel-icon-active" : ""} size={16} />
                    ) : (
                        <Upload className={activeCount > 0 ? "transfer-panel-icon-active" : ""} size={16} />
                    )}
                    <span>
                        {titleText} ({activeCount > 0 ? `${activeCount} active` : `${completedCount} done`})
                    </span>
                </div>
                <div className="transfer-panel-actions">
                    {completedCount > 0 && onClearCompleted && (
                        <button onClick={onClearCompleted} className="transfer-panel-btn-action">
                            Clear finished
                        </button>
                    )}
                    <button
                        onClick={() => setCollapsed(!collapsed)}
                        className="transfer-panel-btn-icon"
                        title={collapsed ? "Expand panel" : "Collapse panel"}
                    >
                        {collapsed ? <ChevronUp size={16} /> : <ChevronDown size={16} />}
                    </button>
                </div>
            </div>

            {/* List Body */}
            {!collapsed && (
                <div className="transfer-panel-body">
                    {items.map((item) => (
                        <div key={item.id} className="transfer-panel-item">
                            <div className="transfer-panel-item-header">
                                <div className="transfer-panel-file-info">
                                    {item.direction === "download" ? (
                                        <Download className="transfer-panel-file-icon" size={15} />
                                    ) : (
                                        <FileText className="transfer-panel-file-icon" size={15} />
                                    )}
                                    <span className="transfer-panel-file-name" title={item.fileName}>
                                        {item.fileName}
                                    </span>
                                </div>
                                <div className="transfer-panel-status-group">
                                    {item.status === "uploading" && (
                                        <span className="transfer-panel-badge transfer-panel-badge-uploading">{item.percent}%</span>
                                    )}
                                    {item.status === "completed" && <CheckCircle2 className="transfer-panel-badge-success" size={15} />}
                                    {item.status === "error" && <AlertCircle className="transfer-panel-badge-error" size={15} />}
                                    {item.status === "paused" && (
                                        <span className="transfer-panel-badge transfer-panel-badge-paused">PAUSED</span>
                                    )}
                                    {onPauseToggle && (item.status === "uploading" || item.status === "paused") && (
                                        <button
                                            onClick={() => onPauseToggle(item.id)}
                                            className="transfer-panel-btn-icon"
                                            title={item.status === "paused" ? "Resume" : "Pause"}
                                        >
                                            {item.status === "paused" ? <Play size={14} /> : <Pause size={14} />}
                                        </button>
                                    )}
                                    {onCancel && (item.status === "uploading" || item.status === "paused") && (
                                        <button
                                            onClick={() => onCancel(item.id)}
                                            className="transfer-panel-btn-icon"
                                            title="Cancel transfer"
                                        >
                                            <X size={14} />
                                        </button>
                                    )}
                                </div>
                            </div>

                            {/* Progress bar line */}
                            <div className="transfer-panel-progress-bg">
                                <div
                                    className={`transfer-panel-progress-fill ${
                                        item.status === "completed"
                                            ? "transfer-panel-progress-fill-completed"
                                            : item.status === "error"
                                              ? "transfer-panel-progress-fill-error"
                                              : item.status === "paused"
                                                ? "transfer-panel-progress-fill-paused"
                                                : "transfer-panel-progress-fill-active"
                                    }`}
                                    style={{ width: `${item.status === "completed" ? 100 : item.percent}%` }}
                                />
                            </div>

                            {/* Sub details */}
                            <div className="transfer-panel-meta">
                                <span>
                                    {formatSize(item.uploadedBytes || Math.round((item.fileSize * item.percent) / 100))} /{" "}
                                    {formatSize(item.fileSize)}
                                </span>
                                <span>
                                    {item.status === "uploading" &&
                                        (item.direction === "download"
                                            ? "Downloading..."
                                            : item.uploadedChunks && item.totalChunks
                                              ? `Chunk ${item.uploadedChunks}/${item.totalChunks}`
                                              : "Uploading...")}
                                    {item.status === "completed" && "Done"}
                                    {item.status === "error" && (item.errorMessage || "Failed")}
                                    {item.status === "paused" && "Paused"}
                                </span>
                            </div>
                        </div>
                    ))}
                </div>
            )}
        </div>
    );
};
