import type { DriveMoveItem } from "@/pages/drive/types";
import { isDriveMoveItem } from "@/pages/drive/types";

const DRIVE_MOVE_MIME = "application/x-sgrn-drive-item+json";
const LEGACY_MOVE_MIME = "application/json";

export function writeDriveMoveItem(transfer: DataTransfer, item: DriveMoveItem): void {
    transfer.setData(DRIVE_MOVE_MIME, JSON.stringify(item));
    // Keep the legacy type for compatibility with tabs running an older bundle.
    transfer.setData(LEGACY_MOVE_MIME, JSON.stringify(item));
    transfer.effectAllowed = "move";
}

export function readDriveMoveItem(transfer: DataTransfer): DriveMoveItem | null {
    const serialized = transfer.getData(DRIVE_MOVE_MIME) || transfer.getData(LEGACY_MOVE_MIME);
    if (!serialized) return null;

    try {
        const parsed: unknown = JSON.parse(serialized);
        return isDriveMoveItem(parsed) ? parsed : null;
    } catch {
        return null;
    }
}
