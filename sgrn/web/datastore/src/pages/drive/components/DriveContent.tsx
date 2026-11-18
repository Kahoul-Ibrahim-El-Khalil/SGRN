import type { DriveFile, DriveFolder } from "@sgrn/types";
import { FileGrid, type UploadNewVersionHandler } from "@/pages/drive/components/FileGrid";
import { FileList } from "@/pages/drive/components/FileList";
import type { DriveItem, DriveListing, DriveMoveItem, DriveItemType } from "@/pages/drive/types";
import type { DriveSortKey, DriveSortOrder } from "@/pages/drive/api";
import { PaginationControls } from "@/pages/drive/components/PaginationControls";

interface DriveContentProps {
    listing: DriveListing;
    viewMode: "grid" | "list";
    currentPage: number;
    debouncedSearch: string;
    sortBy: DriveSortKey;
    sortOrder: DriveSortOrder;
    onSort: (key: DriveSortKey) => void;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelect: (item: DriveMoveItem) => void;
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete: (item: DriveItem, type: DriveItemType) => void;
    onRename: (item: DriveItem, type: DriveItemType) => void;
    onInfo: (item: DriveItem) => void;
    onPageChange: (page: number) => void;
    onMoveItems: (items: DriveMoveItem[], targetId: number) => void;
    onUploadNewVersion?: UploadNewVersionHandler;
}

export function DriveContent({
    listing,
    viewMode,
    currentPage,
    debouncedSearch,
    sortBy,
    sortOrder,
    onSort,
    isSelected,
    onToggleSelect,
    onFolderClick,
    onFolderDownload,
    onFileClick,
    onDownloadDecompressed,
    onPreview,
    onDelete,
    onRename,
    onInfo,
    onPageChange,
    onMoveItems,
    onUploadNewVersion,
}: DriveContentProps) {
    const canWrite = listing.capabilities?.can_write !== false;
    const canDelete = listing.capabilities?.can_delete !== false;
    const sharedProps = {
        folders: listing.folders,
        files: listing.files,
        onFolderClick,
        onFolderDownload,
        onFileClick,
        onDownloadDecompressed,
        onPreview,
        onDelete: canDelete ? onDelete : undefined,
        onRename: canWrite ? onRename : undefined,
        onInfo,
        isSelected,
        onToggleSelection: onToggleSelect,
        onMoveItems: canWrite ? onMoveItems : undefined,
        searchQuery: debouncedSearch,
        sortBy,
        sortOrder,
        onSort,
    };

    return (
        <>
            {viewMode === "grid" ? (
                <FileGrid {...sharedProps} onUploadNewVersion={canWrite ? onUploadNewVersion : undefined} />
            ) : (
                <FileList {...sharedProps} />
            )}

            {listing.total_pages > 1 && (
                <PaginationControls
                    currentPage={currentPage}
                    totalPages={listing.total_pages}
                    onPrev={() => onPageChange(Math.max(1, currentPage - 1))}
                    onNext={() => onPageChange(Math.min(listing.total_pages, currentPage + 1))}
                />
            )}
        </>
    );
}
