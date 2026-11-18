import { Download, Edit3, Info, Trash2 } from "lucide-react";
import type { DriveFolder } from "@sgrn/types";
import type { DriveItem } from "@/pages/drive/types";
import { FileListRowActionButton } from "./FileListRowActionButton";

interface FileListFolderActionsProps {
    folder: DriveFolder;
    onFolderDownload?: (folder: DriveFolder) => void;
    onInfo?: (item: DriveItem, type: "file" | "folder") => void;
    onRename?: (item: DriveItem, type: "file" | "folder") => void;
    onDelete?: (item: DriveItem, type: "file" | "folder") => void;
}

export function FileListFolderActions({ folder, onFolderDownload, onInfo, onRename, onDelete }: FileListFolderActionsProps) {
    return (
        <div className="drive-row-actions opacity-0 group-hover:opacity-100 transition-opacity">
            {onFolderDownload && (
                <FileListRowActionButton icon={Download} label="Download folder" onClick={() => onFolderDownload(folder)} />
            )}
            {onInfo && <FileListRowActionButton icon={Info} label="Folder information" onClick={() => onInfo(folder, "folder")} />}
            {onRename && <FileListRowActionButton icon={Edit3} label="Rename folder" onClick={() => onRename(folder, "folder")} />}
            {onDelete && <FileListRowActionButton icon={Trash2} label="Delete folder" danger onClick={() => onDelete(folder, "folder")} />}
        </div>
    );
}
