import { Download, Edit3, Eye, Info, Trash2 } from "lucide-react";
import type { DriveFile } from "@sgrn/types";
import type { DriveItem } from "@/pages/drive/types";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";
import { FileListRowActionButton } from "./FileListRowActionButton";

interface FileListFileActionsProps {
    file: DriveFile;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete?: (item: DriveItem, type: "file" | "folder") => void;
    onRename?: (item: DriveItem, type: "file" | "folder") => void;
    onInfo?: (item: DriveItem, type: "file" | "folder") => void;
}

export function FileListFileActions({
    file,
    onFileClick,
    onDownloadDecompressed,
    onPreview,
    onDelete,
    onRename,
    onInfo,
}: FileListFileActionsProps) {
    const hasDecompressedDownload = file.name.endsWith(".zst") && onDownloadDecompressed !== undefined;

    return (
        <div className="drive-row-actions opacity-0 group-hover:opacity-100 transition-opacity">
            {onPreview && <FileListRowActionButton icon={Eye} label="Preview" onClick={() => onPreview(file)} />}
            {onInfo && (
                <FileListRowActionButton icon={Info} label={`File information for ${file.name}`} onClick={() => onInfo(file, "file")} />
            )}
            {onRename && <FileListRowActionButton icon={Edit3} label={`Rename ${file.name}`} onClick={() => onRename(file, "file")} />}

            {hasDecompressedDownload ? (
                <div onClick={(event) => event.stopPropagation()}>
                    <DropdownMenu>
                        <DropdownMenuTrigger asChild>
                            <button
                                type="button"
                                className="drive-row-btn"
                                title="Download options"
                                aria-label={`Download options for ${file.name}`}
                            >
                                <Download size={12} />
                            </button>
                        </DropdownMenuTrigger>
                        <DropdownMenuContent align="end" className="w-48 bg-popover border-border text-popover-foreground shadow-xl">
                            <DropdownMenuItem
                                className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                                onClick={() => onDownloadDecompressed?.(file)}
                            >
                                Download Decompressed
                            </DropdownMenuItem>
                            <DropdownMenuItem
                                className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                                onClick={() => onFileClick(file)}
                            >
                                Download Original (.zst)
                            </DropdownMenuItem>
                        </DropdownMenuContent>
                    </DropdownMenu>
                </div>
            ) : (
                <FileListRowActionButton icon={Download} label={`Download ${file.name}`} onClick={() => onFileClick(file)} />
            )}
            {onDelete && (
                <FileListRowActionButton icon={Trash2} label={`Delete ${file.name}`} danger onClick={() => onDelete(file, "file")} />
            )}
        </div>
    );
}
