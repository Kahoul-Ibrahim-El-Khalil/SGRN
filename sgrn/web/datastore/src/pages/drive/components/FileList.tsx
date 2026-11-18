import { memo } from "react";
import { File as FileIcon } from "lucide-react";
import type { DriveFile, DriveFolder } from "@sgrn/types";
import type { DriveItem, DriveMoveItem } from "@/pages/drive/types";
import type { DriveSortKey, DriveSortOrder } from "@/pages/drive/api";
import { useFileListDragAndDrop } from "@/pages/drive/hooks/useFileListDragAndDrop";
import { FileListFileRow } from "@/pages/drive/components/file-list/FileListFileRow";
import { FileListFolderRow } from "@/pages/drive/components/file-list/FileListFolderRow";
import { FileListTableHeader } from "@/pages/drive/components/file-list/FileListTableHeader";

interface FileListProps {
    folders: DriveFolder[];
    files: DriveFile[];
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete?: (item: DriveItem, type: "file" | "folder") => void;
    onRename?: (item: DriveItem, type: "file" | "folder") => void;
    onInfo?: (item: DriveItem, type: "file" | "folder") => void;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelection: (item: DriveMoveItem) => void;
    onMoveItems?: (items: DriveMoveItem[], targetId: number) => void;
    searchQuery?: string;
    sortBy?: DriveSortKey;
    sortOrder?: DriveSortOrder;
    onSort?: (key: DriveSortKey) => void;
}

export const FileList = memo(function FileList({
    folders,
    files,
    onFolderClick,
    onFolderDownload,
    onFileClick,
    onDownloadDecompressed,
    onPreview,
    onDelete,
    onRename,
    onInfo,
    isSelected,
    onToggleSelection,
    onMoveItems,
    searchQuery = "",
    sortBy = "name",
    sortOrder = "asc",
    onSort,
}: FileListProps) {
    const canMove = Boolean(onMoveItems);
    const { dragOverPath, handleDragStart, handleDragEnd, handleFolderDragOver, handleFolderDragLeave, handleFolderDrop } =
        useFileListDragAndDrop(onMoveItems);

    const isEmpty = folders.length === 0 && files.length === 0;
    if (isEmpty) {
        return (
            <div className="drive-empty">
                <div className="p-8 border-2 border-dashed border-border/20 flex flex-col items-center gap-4">
                    <FileIcon size={48} strokeWidth={1} className="text-muted-foreground/20" />
                    <p className="text-[10px] font-black uppercase tracking-[0.3em]">Directory is empty</p>
                </div>
            </div>
        );
    }

    // Separators only earn their place when there are enough rows to need them.
    const showSeparators = folders.length + files.length > 10;

    return (
        <div className="overflow-x-auto">
            <table className={`drive-list w-full${showSeparators ? " drive-list--lined" : ""}`}>
                <FileListTableHeader sortBy={sortBy} sortOrder={sortOrder} onSort={onSort} />
                <tbody>
                    {folders.map((folder) => (
                        <FileListFolderRow
                            key={`folder-${folder.path}`}
                            folder={folder}
                            searchQuery={searchQuery}
                            isSelected={isSelected}
                            onToggleSelection={onToggleSelection}
                            onFolderClick={onFolderClick}
                            onFolderDownload={onFolderDownload}
                            onDelete={onDelete}
                            onRename={onRename}
                            onInfo={onInfo}
                            canMove={canMove}
                            isDragOver={dragOverPath === folder.path}
                            onDragStart={handleDragStart}
                            onDragEnd={handleDragEnd}
                            onDragOver={handleFolderDragOver}
                            onDragLeave={handleFolderDragLeave}
                            onDrop={handleFolderDrop}
                        />
                    ))}
                    {files.map((file) => (
                        <FileListFileRow
                            key={`file-${file.id}`}
                            file={file}
                            searchQuery={searchQuery}
                            isSelected={isSelected}
                            onToggleSelection={onToggleSelection}
                            onFileClick={onFileClick}
                            onDownloadDecompressed={onDownloadDecompressed}
                            onPreview={onPreview}
                            onDelete={onDelete}
                            onRename={onRename}
                            onInfo={onInfo}
                            canMove={canMove}
                            onDragStart={handleDragStart}
                            onDragEnd={handleDragEnd}
                        />
                    ))}
                </tbody>
            </table>
        </div>
    );
});
