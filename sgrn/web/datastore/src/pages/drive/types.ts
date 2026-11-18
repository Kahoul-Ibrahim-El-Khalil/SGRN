import type { DirectoryListing, DriveFile, DriveFolder } from "@sgrn/types";

export type DriveItemType = "file" | "folder";
export type DriveItem = DriveFile | DriveFolder;

export type DriveCapabilities = {
    can_read?: boolean;
    can_write?: boolean;
    can_delete?: boolean;
    allowed_subpath?: string;
};

/** Capability-aware directory listing returned by the datastore Drive API. */
export type DriveListing = DirectoryListing & {
    capabilities?: DriveCapabilities;
};

export interface DriveMoveItem {
    id: number;
    type: DriveItemType;
    source_path?: string;
    name?: string;
}

/** Narrow an untrusted drag payload before allowing it to trigger a move. */
export function isDriveMoveItem(value: unknown): value is DriveMoveItem {
    if (typeof value !== "object" || value === null) return false;

    const candidate = value as Record<string, unknown>;
    return (
        (candidate.type === "file" || candidate.type === "folder") &&
        typeof candidate.id === "number" &&
        Number.isSafeInteger(candidate.id) &&
        candidate.id > 0 &&
        (candidate.source_path === undefined || typeof candidate.source_path === "string") &&
        (candidate.name === undefined || typeof candidate.name === "string")
    );
}
