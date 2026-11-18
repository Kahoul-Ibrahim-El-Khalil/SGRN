// @/pages/drive/format.ts
//
// Single source of truth for drive presentation formatting. Both the list and
// grid views, the details panel, and version history format sizes/dates and
// build drag items here so the two views cannot drift apart.

import type { DriveFile, DriveFolder } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";

const SIZE_UNITS = ["B", "KB", "MB", "GB", "TB"] as const;

/** Human-readable file size, e.g. 1.4 KB. */
export function formatFileSize(bytes: number): string {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";

    const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), SIZE_UNITS.length - 1);
    return `${(bytes / 1024 ** index).toFixed(index === 0 ? 0 : 1)} ${SIZE_UNITS[index]}`;
}

/** Locale date + time; falls back to the raw string when unparseable. */
export function formatDate(dateString: string | null | undefined): string {
    if (!dateString) return "Unknown";
    try {
        const date = new Date(dateString);
        if (Number.isNaN(date.getTime())) return dateString;

        return date.toLocaleDateString(undefined, {
            year: "numeric",
            month: "short",
            day: "numeric",
            hour: "2-digit",
            minute: "2-digit",
        });
    } catch {
        return dateString;
    }
}

/** Display label for a folder, preferring the server-provided display name. */
export function getFolderLabel(folder: DriveFolder): string {
    return folder.display_name || folder.name;
}

export function toFolderMoveItem(folder: DriveFolder): DriveMoveItem | null {
    if (folder.id == null) return null;

    return {
        id: folder.id,
        type: "folder",
        source_path: folder.path,
        name: getFolderLabel(folder),
    };
}

export function toFileMoveItem(file: DriveFile): DriveMoveItem {
    return {
        id: file.id,
        type: "file",
        source_path: file.path,
        name: file.name,
    };
}
