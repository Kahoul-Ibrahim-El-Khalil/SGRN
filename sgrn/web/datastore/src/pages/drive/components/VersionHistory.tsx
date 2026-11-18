import React from "react";
import { History, Download, RotateCcw, Trash2, Layers } from "lucide-react";
import type { DriveFile } from "@sgrn/types";
import type { StorageScope } from "@/pages/drive/api";
import type { FileVersion } from "@/pages/drive/versioning";
import { useFileVersionHistory } from "@/pages/drive/hooks/useFileVersionHistory";

interface VersionHistoryProps {
    file: DriveFile;
    scope?: StorageScope;
    onChanged?: () => void;
    canWrite?: boolean;
    canDelete?: boolean;
}

const SIZE_UNITS = ["B", "KB", "MB", "GB", "TB"] as const;

function formatSize(bytes: number): string {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < SIZE_UNITS.length - 1) {
        value /= 1024;
        unit += 1;
    }
    return `${Number(value.toFixed(unit === 0 ? 0 : 1))} ${SIZE_UNITS[unit]}`;
}

function VersionBadge({ children, current = false }: { children: React.ReactNode; current?: boolean }) {
    return (
        <span
            className={`inline-flex items-center px-1.5 py-0.5 text-[10px] font-mono tracking-wide border ${current ? "version-badge--current" : "version-badge"}`}
        >
            {children}
        </span>
    );
}

interface VersionRowProps {
    version: FileVersion;
    busy: boolean;
    canWrite: boolean;
    onDownload: (version: FileVersion) => void;
    onRestore: (version: FileVersion) => void;
}

function VersionRow({ version, busy, canWrite, onDownload, onRestore }: VersionRowProps) {
    return (
        <article className={`version-row${version.is_current ? " version-row--current" : ""}`}>
            <div className="flex items-center justify-between gap-2">
                <div className="flex items-center gap-1.5 flex-wrap">
                    <strong>v{version.version_no}</strong>
                    {version.is_current && <VersionBadge current>CURRENT</VersionBadge>}
                    {!version.is_current && version.storage_kind === "delta" && <VersionBadge>DELTA</VersionBadge>}
                    {!version.is_current && version.storage_kind === "full" && (
                        <VersionBadge>{version.is_checkpoint ? "CHECKPOINT" : "FULL"}</VersionBadge>
                    )}
                    {version.restored_from_version_id !== null && <VersionBadge>RESTORE</VersionBadge>}
                </div>
                <div className="flex items-center gap-1">
                    <IconButton title={`Download version ${version.version_no}`} disabled={busy} onClick={() => onDownload(version)}>
                        <Download size={15} />
                    </IconButton>
                    {canWrite && !version.is_current && (
                        <IconButton title={`Restore version ${version.version_no}`} disabled={busy} onClick={() => onRestore(version)}>
                            <RotateCcw size={15} />
                        </IconButton>
                    )}
                </div>
            </div>
            <div className="mt-1 text-xs text-muted-foreground">
                {formatDate(version.created_at)} · {formatSize(version.original_size)}
                {version.storage_kind === "delta" ? ` · stored as ${formatSize(version.stored_size)} delta` : ""}
                {version.is_encrypted ? " · encrypted" : ""}
            </div>
            {version.comment && <p className="mt-1 text-sm break-words">{version.comment}</p>}
        </article>
    );
}

function formatDate(value: string): string {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? value : date.toLocaleString();
}

function IconButton({
    title,
    disabled = false,
    onClick,
    children,
}: {
    title: string;
    disabled?: boolean;
    onClick: () => void;
    children: React.ReactNode;
}) {
    return (
        <button type="button" title={title} aria-label={title} disabled={disabled} onClick={onClick} className="version-icon-btn">
            {children}
        </button>
    );
}

export function VersionHistory({ file, scope = "personal", onChanged, canWrite = true, canDelete = true }: VersionHistoryProps) {
    const { state, restore, download, prune, setKeepInput } = useFileVersionHistory({ file, scope, onChanged });
    const { versions, loading, busyVersionNo, error, keepInput } = state;
    const storedBytes = versions.reduce((total, version) => total + version.stored_size, 0);
    const logicalBytes = versions.reduce((total, version) => total + version.original_size, 0);
    const busy = busyVersionNo !== null;

    const handleRestore = (version: FileVersion) => {
        const confirmed = window.confirm(`Restore version ${version.version_no}? It becomes a new current version; nothing is lost.`);
        if (confirmed) void restore(version);
    };

    const handlePrune = () => {
        const keep = Math.max(1, Math.floor(Number.isFinite(keepInput) ? keepInput : 1));
        const confirmed = window.confirm(`Permanently delete all but the newest ${keep} versions? This cannot be undone.`);
        if (confirmed) void prune();
    };

    return (
        <section className="version-history" aria-label="File version history">
            <header className="flex items-center gap-2 font-semibold mb-2">
                <History size={16} />
                <span>Versions</span>
                {versions.length > 0 && <span className="text-sm text-muted-foreground font-normal">({versions.length})</span>}
            </header>

            {versions.length > 1 && (
                <div className="flex items-start gap-2 text-xs text-muted-foreground mb-3">
                    <Layers size={13} className="mt-0.5 shrink-0" />
                    <span>
                        {formatSize(storedBytes)} stored for {formatSize(logicalBytes)} of history
                        {logicalBytes > storedBytes ? ` (${Math.round((1 - storedBytes / logicalBytes) * 100)}% saved by deltas)` : ""}
                    </span>
                </div>
            )}

            {error && (
                <p role="alert" className="text-sm mb-2 break-words version-history-error">
                    {error}
                </p>
            )}
            {loading && <p className="text-sm text-muted-foreground">Loading versions…</p>}
            {!loading && !error && versions.length === 0 && (
                <p className="text-sm text-muted-foreground">
                    No history yet. Uploading a replacement to this file's path creates a new version.
                </p>
            )}

            {versions.length > 0 && (
                <div className="flex flex-col gap-2">
                    {versions.map((version) => (
                        <VersionRow
                            key={version.version_id}
                            version={version}
                            busy={busy}
                            onDownload={(item) => void download(item)}
                            onRestore={handleRestore}
                            canWrite={canWrite}
                        />
                    ))}
                </div>
            )}

            {canDelete && versions.length > 1 && (
                <div className="version-retention">
                    <label htmlFor="drive-version-retention">Keep newest</label>
                    <input
                        id="drive-version-retention"
                        type="number"
                        min={1}
                        step={1}
                        inputMode="numeric"
                        value={keepInput}
                        disabled={busy}
                        onChange={(event) => setKeepInput(event.currentTarget.value === "" ? 1 : Number(event.currentTarget.value))}
                        className="version-retention-input"
                    />
                    <IconButton
                        title="Delete older versions"
                        disabled={busy || !Number.isFinite(keepInput) || keepInput < 1}
                        onClick={handlePrune}
                    >
                        <Trash2 size={15} />
                    </IconButton>
                </div>
            )}
        </section>
    );
}
