// @/pages/drive/components/VersionHistory.tsx
//
// Version history for a single file, shown as a dropdown under a toggle
// button (it lives inside the details dialog, but is not part of its body).
//
// Layout of this file:
//   VersionHistory        – toggle button + collapsible region (public entry)
//   VersionHistoryPanel   – owns the data hook; only mounted while open, so
//                           the versions request is made on demand
//   VersionList / VersionRow / VersionBadge – presentation
//   StorageSavings        – "x stored for y of history" summary
//   RetentionControl      – "keep newest N" + prune button

import { useId, useState, type ChangeEvent, type ReactNode } from "react";
import { ChevronDown, Download, History, Layers, RotateCcw, Trash2 } from "lucide-react";
import type { DriveFile } from "@sgrn/types";
import type { StorageScope } from "@/pages/drive/api";
import type { FileVersion } from "@/pages/drive/versioning";
import { formatDate, formatFileSize } from "@/pages/drive/format";
import { normalizeKeepCount, useFileVersionHistory } from "@/pages/drive/hooks/useFileVersionHistory";

interface VersionHistoryProps {
    file: DriveFile;
    scope?: StorageScope;
    onChanged?: () => void;
    canWrite?: boolean;
    canDelete?: boolean;
}

// ─── helpers ─────────────────────────────────────────────────────────────────

/** Badge text for how a non-current version is stored, or null for the current one. */
function storageKindLabel(version: FileVersion): string | null {
    if (version.is_current) return null;
    if (version.storage_kind === "delta") return "DELTA";
    return version.is_checkpoint ? "CHECKPOINT" : "FULL";
}

/** "Nov 19, 2026 · 1.4 KB · stored as 200 B delta · encrypted" */
function describeVersion(version: FileVersion): string {
    const parts = [formatDate(version.created_at), formatFileSize(version.original_size)];
    if (version.storage_kind === "delta") parts.push(`stored as ${formatFileSize(version.stored_size)} delta`);
    if (version.is_encrypted) parts.push("encrypted");
    return parts.join(" · ");
}

function isValidKeepCount(text: string): boolean {
    const value = Number(text);
    return text.trim() !== "" && Number.isInteger(value) && value >= 1;
}

// ─── small presentational pieces ─────────────────────────────────────────────

function IconButton({
    title,
    disabled = false,
    onClick,
    children,
}: {
    title: string;
    disabled?: boolean;
    onClick: () => void;
    children: ReactNode;
}) {
    return (
        <button type="button" title={title} aria-label={title} disabled={disabled} onClick={onClick} className="version-icon-btn">
            {children}
        </button>
    );
}

function VersionBadge({ children, current = false }: { children: ReactNode; current?: boolean }) {
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
    const kindLabel = storageKindLabel(version);

    return (
        <article className={`version-row${version.is_current ? " version-row--current" : ""}`}>
            <div className="flex items-center justify-between gap-2">
                <div className="flex items-center gap-1.5 flex-wrap">
                    <strong>v{version.version_no}</strong>
                    {version.is_current && <VersionBadge current>CURRENT</VersionBadge>}
                    {kindLabel && <VersionBadge>{kindLabel}</VersionBadge>}
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
            <div className="mt-1 text-xs text-muted-foreground">{describeVersion(version)}</div>
            {version.comment && <p className="mt-1 text-sm break-words">{version.comment}</p>}
        </article>
    );
}

function StorageSavings({ versions }: { versions: FileVersion[] }) {
    if (versions.length < 2) return null;

    const storedBytes = versions.reduce((total, version) => total + version.stored_size, 0);
    const logicalBytes = versions.reduce((total, version) => total + version.original_size, 0);
    const savedPercent = logicalBytes > storedBytes ? Math.round((1 - storedBytes / logicalBytes) * 100) : 0;

    return (
        <div className="flex items-start gap-2 text-xs text-muted-foreground mb-3">
            <Layers size={13} className="mt-0.5 shrink-0" />
            <span>
                {formatFileSize(storedBytes)} stored for {formatFileSize(logicalBytes)} of history
                {savedPercent > 0 ? ` (${savedPercent}% saved by deltas)` : ""}
            </span>
        </div>
    );
}

interface RetentionControlProps {
    keepInput: number;
    busy: boolean;
    onKeepChange: (keep: number) => void;
    onPrune: () => void;
}

/**
 * Keeps the raw text locally so the field can be cleared and retyped; only a
 * valid whole number >= 1 is pushed to the hook, and the prune button is
 * disabled for anything else.
 */
function RetentionControl({ keepInput, busy, onKeepChange, onPrune }: RetentionControlProps) {
    const inputId = useId();
    const [text, setText] = useState(String(keepInput));

    const handleChange = (event: ChangeEvent<HTMLInputElement>) => {
        const next = event.currentTarget.value;
        setText(next);
        if (isValidKeepCount(next)) onKeepChange(Number(next));
    };

    return (
        <div className="version-retention">
            <label htmlFor={inputId}>Keep newest</label>
            <input
                id={inputId}
                type="number"
                min={1}
                step={1}
                inputMode="numeric"
                value={text}
                disabled={busy}
                onChange={handleChange}
                className="version-retention-input"
            />
            <IconButton title="Delete older versions" disabled={busy || !isValidKeepCount(text)} onClick={onPrune}>
                <Trash2 size={15} />
            </IconButton>
        </div>
    );
}

// ─── panel (data + actions) ──────────────────────────────────────────────────

function VersionHistoryPanel({ file, scope, onChanged, canWrite, canDelete }: Required<VersionHistoryProps>) {
    const { state, restore, download, prune, setKeepInput } = useFileVersionHistory({ file, scope, onChanged });
    const { versions, loading, busyVersionNo, error, keepInput } = state;
    const busy = busyVersionNo !== null;

    const handleRestore = (version: FileVersion) => {
        const confirmed = window.confirm(`Restore version ${version.version_no}? It becomes a new current version; nothing is lost.`);
        if (confirmed) void restore(version);
    };

    const handlePrune = () => {
        const keep = normalizeKeepCount(keepInput);
        const confirmed = window.confirm(`Permanently delete all but the newest ${keep} versions? This cannot be undone.`);
        if (confirmed) void prune();
    };

    return (
        <>
            <StorageSavings versions={versions} />

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
                            canWrite={canWrite}
                            onDownload={(item) => void download(item)}
                            onRestore={handleRestore}
                        />
                    ))}
                </div>
            )}

            {canDelete && versions.length > 1 && (
                <RetentionControl keepInput={keepInput} busy={busy} onKeepChange={setKeepInput} onPrune={handlePrune} />
            )}
        </>
    );
}

// ─── public entry: button + dropdown ─────────────────────────────────────────

export function VersionHistory({ file, scope = "personal", onChanged, canWrite = true, canDelete = true }: VersionHistoryProps) {
    const [open, setOpen] = useState(false);
    const regionId = useId();

    return (
        <section className="version-history" aria-label="File version history">
            <button
                type="button"
                className={`version-toggle${open ? " version-toggle--open" : ""}`}
                aria-expanded={open}
                aria-controls={regionId}
                onClick={() => setOpen((prev) => !prev)}
            >
                <History size={16} />
                <span className="version-toggle-label">Version history</span>
                <ChevronDown size={16} className="version-toggle-chevron" />
            </button>

            {open && (
                <div id={regionId} className="version-dropdown">
                    <VersionHistoryPanel
                        file={file}
                        scope={scope}
                        onChanged={onChanged ?? noop}
                        canWrite={canWrite}
                        canDelete={canDelete}
                    />
                </div>
            )}
        </section>
    );
}

function noop(): void {}
