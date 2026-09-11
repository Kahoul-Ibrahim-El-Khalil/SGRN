export type DriveItemType = "file" | "folder";

export interface DriveMoveItem {
    id: number;
    type: DriveItemType;
    source_path?: string;
    name?: string;
}
