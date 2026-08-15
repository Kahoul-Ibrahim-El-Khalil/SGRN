// @/pages/drive/page.tsx
//
// Architecture overview:
//   Custom Hooks (logic)
//     useDriveScope        – scope/path navigation state
//     useDriveListing      – data fetching, search, pagination
//     useDriveSelection    – multi-select state
//     useDriveUpload       – file & folder upload flows
//     useDriveActions      – create folder, delete, rename, navigate
//
//   Local Components (UI)
//     DriveHeader          – title, scope tabs, breadcrumb, search, refresh
//     DriveToolbar         – view-mode toggle + upload / new-folder buttons
//     NewFolderPrompt      – inline create-folder input
//     DriveContent         – grid/list + pagination
//     PaginationControls   – prev/next page row

import { useState, useCallback, useEffect, useRef } from "react";
import { Loader2, Upload, FolderPlus, Shield, LayoutGrid, List, Search, ChevronLeft, ChevronRight } from "lucide-react";
import { PageLayout } from "@/components/PageLayout";
import { Breadcrumb } from "@/pages/drive/components/Breadcrumb";
import { FileGrid } from "@/pages/drive/components/FileGrid";
import { FileList } from "@/pages/drive/components/FileList";
import {
    listDirectory,
    downloadDriveFile,
    uploadDriveFile,
    uploadDriveFilesBatch,
    createDirectory,
    deleteDriveItem,
    renameItem,
    bulkAction,
    type StorageScope,
} from "@/pages/drive/api";
import { RenameModal } from "@/pages/drive/components/RenameModal";
import { DetailsPanel } from "@/pages/drive/components/DetailsPanel";
import { SelectionBar } from "@/pages/drive/components/SelectionBar";
import { FilePreviewModal } from "@/pages/drive/components/FilePreviewModal";
import { useEvent } from "@/contexts/EventContext";
import { useAuth } from "@/contexts/AuthContext";
import type { DirectoryListing, DriveFolder, DriveFile } from "@sgrn/types";
import { isError } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";

// ─── helpers ────────────────────────────────────────────────────────────────

function getDriveItemKey(item: DriveMoveItem) {
    return `${item.type}:${item.id}`;
}

const PAGE_SIZE = 20;

// ─── custom hooks ────────────────────────────────────────────────────────────

/** Manages the active storage scope and current path. */
function useDriveScope(isAdmin: boolean) {
    const [currentScope, setCurrentScope] = useState<StorageScope>("personal");
    const [currentPath, setCurrentPath] = useState("/");

    const isNamespaceRoot = isAdmin && currentScope !== "personal" && currentPath === "/";

    const switchScope = useCallback((scope: StorageScope) => {
        setCurrentScope(scope);
        setCurrentPath("/");
    }, []);

    const navigate = useCallback((path: string) => {
        setCurrentPath(path);
    }, []);

    return { currentScope, currentPath, isNamespaceRoot, switchScope, navigate };
}

/** Handles fetching the directory listing with search and pagination. */
function useDriveListing(currentPath: string, currentScope: StorageScope, showEvent: ReturnType<typeof useEvent>["showEvent"]) {
    const [listing, setListing] = useState<DirectoryListing | null>(null);
    const [loading, setLoading] = useState(false);
    const [currentPage, setCurrentPage] = useState(1);
    const [searchQuery, setSearchQuery] = useState("");
    const [debouncedSearch, setDebouncedSearch] = useState("");
    const [initialized, setInitialized] = useState(false);

    // reset page when path / scope change
    useEffect(() => {
        setCurrentPage(1);
    }, [currentPath, currentScope]);

    // debounce search input
    useEffect(() => {
        const t = setTimeout(() => {
            setDebouncedSearch(searchQuery);
            setCurrentPage(1);
        }, 300);
        return () => clearTimeout(t);
    }, [searchQuery]);

    const fetchListing = useCallback(
        async (path = currentPath, scope = currentScope, page = currentPage, search = debouncedSearch) => {
            setLoading(true);
            setListing(null);
            const result = await listDirectory(path, scope, page, PAGE_SIZE, search);
            if (isError(result)) {
                showEvent("error", result.error);
                setListing({
                    path,
                    folders: [],
                    files: [],
                    total_items: 0,
                    total_folders: 0,
                    total_files: 0,
                    page: 1,
                    page_size: PAGE_SIZE,
                    total_pages: 1,
                });
            } else {
                setListing(result.data);
            }
            setLoading(false);
            return isError(result) ? null : result.data;
        },
        [currentPath, currentScope, currentPage, debouncedSearch, showEvent],
    );

    // initial + reactive fetch
    useEffect(() => {
        if (!initialized) return;
        fetchListing(currentPath, currentScope, currentPage, debouncedSearch);
    }, [initialized, currentPath, currentScope, currentPage, debouncedSearch, fetchListing]);

    return {
        listing,
        loading,
        currentPage,
        searchQuery,
        debouncedSearch,
        setCurrentPage,
        setSearchQuery,
        fetchListing,
        setInitialized,
    };
}

/** Manages multi-item selection. */
function useDriveSelection() {
    const [selectedItems, setSelectedItems] = useState<DriveMoveItem[]>([]);

    const clear = useCallback(() => setSelectedItems([]), []);

    const isSelected = useCallback(
        (item: DriveMoveItem) => selectedItems.some((s) => s.id === item.id && s.type === item.type),
        [selectedItems],
    );

    const toggle = useCallback((item: DriveMoveItem) => {
        setSelectedItems((prev) => {
            const key = getDriveItemKey(item);
            const exists = prev.some((s) => getDriveItemKey(s) === key);
            return exists ? prev.filter((s) => getDriveItemKey(s) !== key) : [...prev, item];
        });
    }, []);

    return { selectedItems, clear, isSelected, toggle };
}

/** Handles single-file and folder (batch) uploads. */
function useDriveUpload(
    currentPath: string,
    currentScope: StorageScope,
    onComplete: () => void,
    showEvent: ReturnType<typeof useEvent>["showEvent"],
) {
    const [uploading, setUploading] = useState(false);
    const fileInputRef = useRef<HTMLInputElement>(null);
    const folderInputRef = useRef<HTMLInputElement>(null);

    const handleFileSelect = useCallback(
        async (e: React.ChangeEvent<HTMLInputElement>) => {
            const files = e.target.files;
            if (!files?.length) return;

            const list = Array.from(files);
            setUploading(true);

            if (list.length > 1) {
                const result = await uploadDriveFilesBatch(currentPath, list, currentScope, () => {});
                if (isError(result)) {
                    showEvent("error", result.error);
                } else {
                    const { success_count, fail_count } = result.data;
                    showEvent(
                        success_count > 0 ? "success" : "error",
                        success_count > 0
                            ? `${success_count} file${success_count > 1 ? "s" : ""} uploaded${fail_count > 0 ? `, ${fail_count} failed` : ""}`
                            : `Failed to upload ${fail_count} files`,
                    );
                }
            } else {
                const result = await uploadDriveFile(currentPath, list[0], currentScope, () => {});
                if (isError(result)) showEvent("error", `${list[0].name}: ${result.error}`);
                else showEvent("success", `${list[0].name} uploaded successfully`);
            }

            setUploading(false);
            if (fileInputRef.current) fileInputRef.current.value = "";
            onComplete();
        },
        [currentPath, currentScope, showEvent, onComplete],
    );

    const handleFolderSelect = useCallback(
        async (e: React.ChangeEvent<HTMLInputElement>) => {
            const files = e.target.files;
            if (!files?.length) return;

            const list = Array.from(files);
            setUploading(true);
            showEvent("info", `Uploading folder structure (${list.length} files)...`);

            const result = await uploadDriveFilesBatch(currentPath, list, currentScope, () => {});
            if (isError(result)) {
                showEvent("error", result.error);
            } else {
                const { success_count, fail_count } = result.data;
                showEvent(
                    success_count > 0 ? "success" : "error",
                    success_count > 0
                        ? `Folder uploaded: ${success_count} files preserved${fail_count > 0 ? `, ${fail_count} failed` : ""}`
                        : "Folder upload failed",
                );
            }

            setUploading(false);
            if (folderInputRef.current) folderInputRef.current.value = "";
            onComplete();
        },
        [currentPath, currentScope, showEvent, onComplete],
    );

    const uploadFilesDirect = useCallback(
        async (list: File[]) => {
            if (!list.length) return;
            setUploading(true);

            if (list.length > 1) {
                const result = await uploadDriveFilesBatch(currentPath, list, currentScope, () => {});
                if (isError(result)) {
                    showEvent("error", result.error);
                } else {
                    const { success_count, fail_count } = result.data;
                    showEvent(
                        success_count > 0 ? "success" : "error",
                        success_count > 0
                            ? `${success_count} file${success_count > 1 ? "s" : ""} uploaded${fail_count > 0 ? `, ${fail_count} failed` : ""}`
                            : `Failed to upload ${fail_count} files`,
                    );
                }
            } else {
                const result = await uploadDriveFile(currentPath, list[0], currentScope, () => {});
                if (isError(result)) showEvent("error", `${list[0].name}: ${result.error}`);
                else showEvent("success", `${list[0].name} uploaded successfully`);
            }

            setUploading(false);
            onComplete();
        },
        [currentPath, currentScope, showEvent, onComplete],
    );

    return { uploading, fileInputRef, folderInputRef, handleFileSelect, handleFolderSelect, uploadFilesDirect };
}

function useDriveActions(
    currentPath: string,
    currentScope: StorageScope,
    onRefresh: () => void,
    onClearSelection: () => void,
    showEvent: ReturnType<typeof useEvent>["showEvent"],
    listing?: any,
) {
    const [showNewFolder, setShowNewFolder] = useState(false);
    const [newFolderName, setNewFolderName] = useState("");
    const [creatingFolder, setCreatingFolder] = useState(false);
    const [renamingItem, setRenamingItem] = useState<{ id: number; type: "file" | "folder"; name: string } | null>(null);
    const [infoItem, setInfoItem] = useState<DriveFolder | DriveFile | null>(null);
    const newFolderInputRef = useRef<HTMLInputElement>(null);

    // auto-focus new-folder input
    useEffect(() => {
        if (showNewFolder) setTimeout(() => newFolderInputRef.current?.focus(), 50);
    }, [showNewFolder]);

    const openNewFolder = useCallback(() => {
        const canWrite = listing?.capabilities?.can_write !== false;
        if (!canWrite) {
            showEvent("error", "Write permission denied in this workspace");
            return;
        }
        setNewFolderName("");
        setShowNewFolder(true);
    }, [listing, showEvent]);

    const submitNewFolder = useCallback(async () => {
        const canWrite = listing?.capabilities?.can_write !== false;
        if (!canWrite) {
            showEvent("error", "Write permission denied in this workspace");
            return;
        }

        const name = newFolderName.trim();
        if (!name) return;
        if (name.includes("/") || name === "." || name === "..") {
            showEvent("error", "Invalid folder name");
            return;
        }

        const fullPath = currentPath === "/" ? `/${name}` : `${currentPath}/${name}`;
        setCreatingFolder(true);
        const result = await createDirectory(fullPath, currentScope);
        if (isError(result)) {
            showEvent("error", result.error);
        } else {
            showEvent("success", `Folder "${name}" created`);
            setShowNewFolder(false);
            setNewFolderName("");
            onRefresh();
            onClearSelection();
        }
        setCreatingFolder(false);
    }, [newFolderName, currentPath, currentScope, showEvent, onRefresh, onClearSelection, listing]);

    const handleDelete = useCallback(
        async (item: DriveFolder | DriveFile, type: "file" | "folder") => {
            const canDelete = listing?.capabilities?.can_delete !== false;
            if (!canDelete) {
                showEvent("error", "Delete permission denied in this workspace");
                return;
            }

            const id = (item as any).id;
            if (!id) {
                showEvent("error", "Cannot delete virtual folders");
                return;
            }
            if (!confirm(`Are you sure you want to delete this ${type}?`)) return;

            showEvent("info", `Deleting ${item.name}...`);
            const result = await deleteDriveItem(id, type as any, currentScope);
            if (isError(result)) showEvent("error", result.error);
            else {
                showEvent("success", `${item.name} deleted`);
                onRefresh();
                onClearSelection();
            }
        },
        [currentScope, showEvent, onRefresh, onClearSelection, listing],
    );

    const handleBulkDelete = useCallback(
        async (selectedItems: DriveMoveItem[]) => {
            const canDelete = listing?.capabilities?.can_delete !== false;
            if (!canDelete) {
                showEvent("error", "Delete permission denied in this workspace");
                return;
            }

            if (!selectedItems.length) return;
            if (!confirm(`Are you sure you want to delete ${selectedItems.length} items?`)) return;

            const items = selectedItems.map(({ id, type }) => ({ id, type }));
            const result = await bulkAction("delete", items);

            if (isError(result)) {
                showEvent("error", result.error);
            } else {
                const data = result.data as any;
                showEvent("success", `Processed ${data.success_count} of ${data.total} items`);
                onClearSelection();
                onRefresh();
            }
        },
        [showEvent, onRefresh, onClearSelection, listing],
    );

    const handleRenameConfirm = useCallback(
        async (newName: string) => {
            const canWrite = listing?.capabilities?.can_write !== false;
            if (!canWrite) {
                showEvent("error", "Rename permission denied in this workspace");
                return;
            }

            if (!renamingItem) return;
            const result = await renameItem(renamingItem.id, renamingItem.type, newName);
            if (isError(result)) showEvent("error", result.error);
            else {
                showEvent("success", "Item renamed successfully");
                onRefresh();
            }
        },
        [renamingItem, showEvent, onRefresh, listing],
    );

    const handleFileClick = useCallback(
        async (file: DriveFile) => {
            showEvent("info", `Downloading ${file.name}...`);
            const result = await downloadDriveFile(file.path, file.name, currentScope);
            if (isError(result)) showEvent("error", result.error);
            else showEvent("success", `${file.name} downloaded successfully`);
        },
        [showEvent, currentScope],
    );

    const handleFileDownloadDecompressed = useCallback(
        async (file: DriveFile) => {
            showEvent("info", `Decompressing & downloading ${file.name}...`);
            const { fetchDriveFileContent } = await import("@/pages/drive/api");
            const result = await fetchDriveFileContent(file.path, currentScope);
            if (isError(result)) {
                showEvent("error", result.error);
                return;
            }

            try {
                const view = new Uint8Array(result.data as ArrayBuffer);
                const { decompress } = await import("fzstd");
                const decompressed = decompress(view) as any;
                const blob = new Blob([decompressed], { type: "application/octet-stream" });
                const url = URL.createObjectURL(blob);
                const a = document.createElement("a");
                a.href = url;
                const newName = file.name.endsWith(".zst") ? file.name.slice(0, -4) : file.name;
                a.download = newName;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
                showEvent("success", `${newName} decompressed successfully`);
            } catch (e) {
                showEvent("error", "Failed to decompress file.");
            }
        },
        [showEvent, currentScope],
    );

    const handleFolderDownload = useCallback(
        async (folder: DriveFolder) => {
            showEvent("info", `Preparing to download folder ${folder.name}...`);

            try {
                const JSZip = (await import("jszip")).default;
                const { fetchDriveFileContent, listDirectory } = await import("@/pages/drive/api");

                const zip = new JSZip();
                let downloadedFiles = 0;

                const processFolder = async (path: string, currentZipFolder: any) => {
                    let page = 1;
                    let hasMore = true;
                    while (hasMore) {
                        const result = await listDirectory(path, currentScope, page, 100);
                        if (isError(result)) {
                            console.error(`Failed to list ${path}: ${result.error}`);
                            return;
                        }
                        const listing = result.data as DirectoryListing;

                        // Process files
                        for (const file of listing.files) {
                            showEvent("info", `Downloading ${file.name}...`);

                            const fileRes = await fetchDriveFileContent(file.path, currentScope);
                            if (!isError(fileRes)) {
                                const buffer = fileRes.data as ArrayBuffer;
                                const view = new Uint8Array(buffer);

                                let isZstd = file.name.endsWith(".zst");
                                if (
                                    !isZstd &&
                                    view.length >= 4 &&
                                    view[0] === 0x28 &&
                                    view[1] === 0xb5 &&
                                    view[2] === 0x2f &&
                                    view[3] === 0xfd
                                ) {
                                    isZstd = true;
                                }

                                let dataToSave: Uint8Array | ArrayBuffer = buffer;
                                let fileName = file.name;

                                if (isZstd) {
                                    try {
                                        const { decompress } = await import("fzstd");
                                        dataToSave = decompress(view) as any;
                                        if (fileName.endsWith(".zst")) {
                                            fileName = fileName.slice(0, -4);
                                        }
                                    } catch (e) {
                                        console.error("Failed to decompress", file.name);
                                    }
                                }

                                currentZipFolder.file(fileName, dataToSave);
                                downloadedFiles++;
                            }
                        }

                        // Process subfolders sequentially to avoid swamping the server
                        for (const sub of listing.folders) {
                            if (sub.token) continue; // Skip virtual infrastructure folders if they exist
                            const subFolderZip = currentZipFolder.folder(sub.name);
                            await processFolder(sub.path, subFolderZip);
                        }

                        hasMore = page < listing.total_pages;
                        page++;
                    }
                };

                await processFolder(folder.path, zip.folder(folder.name));

                if (downloadedFiles === 0) {
                    showEvent("info", "Folder is empty.");
                    return;
                }

                showEvent("info", `Zipping ${downloadedFiles} files...`);
                const blob = await zip.generateAsync({ type: "blob" });

                const url = URL.createObjectURL(blob);
                const a = document.createElement("a");
                a.href = url;
                a.download = `${folder.name}.zip`;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
                showEvent("success", `Downloaded ${folder.name}.zip`);
            } catch (err) {
                showEvent("error", `Error creating zip: ${err instanceof Error ? err.message : String(err)}`);
            }
        },
        [showEvent, currentScope],
    );

    const handleMoveItems = useCallback(
        async (items: DriveMoveItem[], targetId: number) => {
            if (!items.length) return;
            showEvent("info", `Moving ${items.length} item${items.length > 1 ? "s" : ""}...`);

            const result = await bulkAction(
                "move",
                items.map((i) => ({ id: i.id, type: i.type })),
                targetId,
            );

            if (isError(result)) {
                showEvent("error", result.error);
            } else {
                const data = result.data as any;
                showEvent("success", `Moved ${data.success_count} of ${data.total} items`);
                onRefresh();
                onClearSelection();
            }
        },
        [showEvent, onRefresh, onClearSelection],
    );

    return {
        showNewFolder,
        newFolderName,
        creatingFolder,
        newFolderInputRef,
        renamingItem,
        infoItem,
        setNewFolderName,
        setShowNewFolder,
        setRenamingItem,
        setInfoItem,
        openNewFolder,
        submitNewFolder,
        handleDelete,
        handleBulkDelete,
        handleRenameConfirm,
        handleFileClick,
        handleFileDownloadDecompressed,
        handleFolderDownload,
        handleMoveItems,
    };
}

// ─── local components ────────────────────────────────────────────────────────

interface DriveHeaderProps {
    currentPath: string;
    trail: DirectoryListing["trail"];
    onNavigate: (path: string) => void;
}

function DriveHeader({ currentPath, trail, onNavigate }: DriveHeaderProps) {
    return (
        <div className="drive-header">
            <Breadcrumb path={currentPath} trail={trail} onNavigate={onNavigate} />
        </div>
    );
}

interface DriveToolbarProps {
    currentScope: StorageScope;
    checkAdmin: boolean;
    onSwitchScope: (scope: StorageScope) => void;
    viewMode: "grid" | "list";
    uploading: boolean;
    loading: boolean;
    searchQuery: string;
    fileInputRef: React.RefObject<HTMLInputElement | null>;
    folderInputRef: React.RefObject<HTMLInputElement | null>;
    onSetViewMode: (mode: "grid" | "list") => void;
    onNewFolder: () => void;
    onFileChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
    onFolderChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
    onSearch: (q: string) => void;
    onRefresh: () => void;
    capabilities?: any;
}

function DriveToolbar({
    currentScope,
    checkAdmin,
    onSwitchScope,
    viewMode,
    uploading,
    loading,
    searchQuery,
    fileInputRef,
    folderInputRef,
    onSetViewMode,
    onNewFolder,
    onFileChange,
    onFolderChange,
    onSearch,
    onRefresh,
    capabilities,
}: DriveToolbarProps) {
    const canWrite = capabilities?.can_write !== false;
    return (
        <div className="drive-toolbar">
            <div className="drive-view-toggle">
                <button
                    onClick={() => onSetViewMode("grid")}
                    className={`drive-view-btn${viewMode === "grid" ? " drive-view-btn-active" : ""}`}
                    title="Grid View"
                >
                    <LayoutGrid size={16} />
                </button>
                <button
                    onClick={() => onSetViewMode("list")}
                    className={`drive-view-btn${viewMode === "list" ? " drive-view-btn-active" : ""}`}
                    title="List View"
                >
                    <List size={16} />
                </button>
            </div>

            <div className="drive-toolbar-sep" />

            <div className="drive-scope-tabs" style={{ flexShrink: 0 }}>
                {(["personal", "automated_services", ...(checkAdmin ? ["users", "domains"] : [])] as StorageScope[]).map((scope) => (
                    <button
                        key={scope}
                        onClick={() => onSwitchScope(scope)}
                        className={`drive-scope-btn ${currentScope === scope ? "drive-scope-btn-active" : ""}`}
                    >
                        {scope !== "personal" && <Shield size={12} />}
                        <span>
                            {scope === "automated_services"
                                ? "Services"
                                : scope === "users"
                                  ? "Users"
                                  : scope === "domains"
                                    ? "Domains"
                                    : "Personal"}
                        </span>
                    </button>
                ))}
            </div>

            <div className="drive-toolbar-sep" />

            <div className="drive-search-wrapper">
                <Search size={14} className="drive-search-icon" />
                <input
                    type="text"
                    placeholder="SEARCH..."
                    className="drive-search-input"
                    value={searchQuery}
                    onChange={(e) => onSearch(e.target.value)}
                />
            </div>

            <div className="drive-toolbar-sep" />

            <div className="drive-toolbar-actions">
                <button
                    onClick={onNewFolder}
                    disabled={!canWrite}
                    className="btn-industrial-secondary drive-toolbar-btn"
                    title={!canWrite ? "Write permission denied in this workspace" : ""}
                >
                    <FolderPlus size={14} /> NEW FOLDER
                </button>

                {/* Folder upload */}
                <button
                    onClick={() => folderInputRef.current?.click()}
                    disabled={uploading || !canWrite}
                    className="btn-industrial-secondary drive-toolbar-btn"
                    title={!canWrite ? "Write permission denied in this workspace" : ""}
                >
                    <FolderPlus size={14} /> UPLOAD FOLDER
                </button>
                <input
                    ref={folderInputRef}
                    type="file"
                    {...({ webkitdirectory: "" } as any)}
                    style={{ display: "none" }}
                    onChange={onFolderChange}
                />

                {/* File upload */}
                <button
                    onClick={() => fileInputRef.current?.click()}
                    disabled={uploading || !canWrite}
                    className="btn-industrial-primary drive-toolbar-btn"
                    title={!canWrite ? "Write permission denied in this workspace" : ""}
                >
                    <Upload size={14} /> UPLOAD FILE
                </button>
                <input ref={fileInputRef} type="file" multiple style={{ display: "none" }} onChange={onFileChange} />

                <button onClick={onRefresh} className="btn-industrial-secondary drive-toolbar-btn drive-refresh-btn" title="Refresh">
                    <Upload size={14} className={loading ? "drive-spin" : ""} style={{ transform: "rotate(180deg)" }} />
                </button>
            </div>
        </div>
    );
}

interface NewFolderPromptProps {
    inputRef: React.RefObject<HTMLInputElement | null>;
    name: string;
    creating: boolean;
    onChange: (name: string) => void;
    onSubmit: () => void;
    onCancel: () => void;
}

function NewFolderPrompt({ inputRef, name, creating, onChange, onSubmit, onCancel }: NewFolderPromptProps) {
    return (
        <div className="mb-4 flex items-center gap-2 bg-muted/10 p-3 border border-border">
            <FolderPlus size={18} className="text-primary" />
            <input
                ref={inputRef}
                type="text"
                className="input-desktop flex-1"
                placeholder="New folder name..."
                value={name}
                onChange={(e) => onChange(e.target.value)}
                onKeyDown={(e) => e.key === "Enter" && onSubmit()}
                disabled={creating}
            />
            <button className="btn-desktop-primary px-4 py-2" onClick={onSubmit} disabled={creating || !name.trim()}>
                {creating ? <Loader2 size={16} className="animate-spin" /> : "CREATE"}
            </button>
            <button className="btn-desktop-secondary px-4 py-2" onClick={onCancel}>
                CANCEL
            </button>
        </div>
    );
}

interface PaginationControlsProps {
    currentPage: number;
    totalPages: number;
    onPrev: () => void;
    onNext: () => void;
}

function PaginationControls({ currentPage, totalPages, onPrev, onNext }: PaginationControlsProps) {
    return (
        <div
            className="drive-pagination"
            style={{
                display: "flex",
                alignItems: "center",
                justifyContent: "center",
                gap: "1rem",
                marginTop: "2rem",
                padding: "1rem 0",
                borderTop: "1px solid rgba(255,255,255,0.05)",
            }}
        >
            <button onClick={onPrev} disabled={currentPage === 1} className="btn-desktop px-3">
                <ChevronLeft size={16} />
            </button>
            <div className="drive-pagination-info">
                PAGE <span className="text-foreground">{currentPage}</span> OF <span className="text-foreground">{totalPages}</span>
            </div>
            <button onClick={onNext} disabled={currentPage === totalPages} className="btn-desktop px-3">
                <ChevronRight size={16} />
            </button>
        </div>
    );
}

interface DriveContentProps {
    listing: DirectoryListing;
    viewMode: "grid" | "list";
    currentPage: number;
    debouncedSearch: string;
    isSelected: (item: DriveMoveItem) => boolean;
    onToggleSelect: (item: DriveMoveItem) => void;
    onFolderClick: (folder: DriveFolder) => void;
    onFolderDownload?: (folder: DriveFolder) => void;
    onFileClick: (file: DriveFile) => void;
    onDownloadDecompressed?: (file: DriveFile) => void;
    onPreview?: (file: DriveFile) => void;
    onDelete: (item: DriveFolder | DriveFile, type: "file" | "folder") => void;
    onRename: (item: DriveFolder | DriveFile, type: "file" | "folder") => void;
    onInfo: (item: DriveFolder | DriveFile) => void;
    onPageChange: (page: number) => void;
    onMoveItems: (items: DriveMoveItem[], targetId: number) => void;
}

function DriveContent({
    listing,
    viewMode,
    currentPage,
    debouncedSearch,
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
}: DriveContentProps) {
    const sharedProps = {
        folders: listing.folders,
        files: listing.files,
        onFolderClick,
        onFolderDownload,
        onFileClick,
        onDownloadDecompressed,
        onPreview,
        onDelete,
        onRename: (item: DriveFolder | DriveFile, type: "file" | "folder") => onRename(item, type),
        onInfo,
        isSelected,
        onToggleSelection: onToggleSelect,
        onMoveItems,
        searchQuery: debouncedSearch,
    };

    return (
        <>
            {viewMode === "grid" ? <FileGrid {...sharedProps} /> : <FileList {...sharedProps} />}

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

// ─── page ────────────────────────────────────────────────────────────────────

export default function DrivePage() {
    const { showEvent } = useEvent();
    const { user } = useAuth();

    const [viewMode, setViewMode] = useState<"grid" | "list">("grid");

    const checkAdmin = user?.role === "admin" || (user?.role as any)?.name === "admin";
    const [previewFile, setPreviewFile] = useState<DriveFile | null>(null);

    // hooks
    const scope = useDriveScope(checkAdmin);
    const listing = useDriveListing(scope.currentPath, scope.currentScope, showEvent);
    const selection = useDriveSelection();

    const refresh = useCallback(() => {
        listing.fetchListing(scope.currentPath, scope.currentScope, listing.currentPage, listing.debouncedSearch);
    }, [listing, scope.currentPath, scope.currentScope]);

    const upload = useDriveUpload(
        scope.currentPath,
        scope.currentScope,
        () => {
            refresh();
            selection.clear();
        },
        showEvent,
    );

    const actions = useDriveActions(scope.currentPath, scope.currentScope, refresh, selection.clear, showEvent, listing.listing);

    // initialize on first mount
    useEffect(() => {
        if (!user) return;
        listing.setInitialized(true);
    }, [user]); // eslint-disable-line react-hooks/exhaustive-deps

    // clear selection on navigation
    useEffect(() => {
        selection.clear();
        actions.setInfoItem(null);
    }, [scope.currentPath, scope.currentScope]); // eslint-disable-line react-hooks/exhaustive-deps

    // hide new-folder prompt when at namespace root
    useEffect(() => {
        if (scope.isNamespaceRoot && actions.showNewFolder) {
            actions.setShowNewFolder(false);
        }
    }, [scope.isNamespaceRoot, actions.showNewFolder]); // eslint-disable-line react-hooks/exhaustive-deps

    const [isDragging, setIsDragging] = useState(false);

    const handleDragOver = useCallback((e: React.DragEvent) => {
        if (e.dataTransfer.types.includes("Files")) {
            e.preventDefault();
            e.stopPropagation();
            setIsDragging(true);
        }
    }, []);

    const handleDragLeave = useCallback((e: React.DragEvent) => {
        e.preventDefault();
        e.stopPropagation();
        setIsDragging(false);
    }, []);

    const handleDrop = useCallback(
        async (e: React.DragEvent) => {
            if (e.dataTransfer.types.includes("Files")) {
                e.preventDefault();
                e.stopPropagation();
                setIsDragging(false);

                if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
                    const filesList = Array.from(e.dataTransfer.files);
                    await upload.uploadFilesDirect(filesList);
                }
            }
        },
        [upload],
    );

    const handleNavigate = useCallback(
        (path: string) => {
            scope.navigate(path);
            selection.clear();
            actions.setInfoItem(null);
        },
        [scope, selection, actions],
    );

    const handleRenameOpen = useCallback(
        (item: DriveFolder | DriveFile, type: "file" | "folder") =>
            actions.setRenamingItem({ id: item.id as number, type, name: item.name }),
        [actions],
    );

    return (
        <PageLayout>
            <div className="drive-page" onDragOver={handleDragOver}>
                {isDragging && (
                    <div className="drive-drag-overlay" onDragOver={handleDragOver} onDragLeave={handleDragLeave} onDrop={handleDrop}>
                        <div className="drive-drag-overlay-card">
                            <Upload size={40} className="drive-drag-overlay-icon" />
                            <h3 className="drive-drag-overlay-title">DROP TO UPLOAD TO SGRN</h3>
                            <p className="drive-drag-overlay-subtitle">
                                Your files will be preserved within <span className="drive-drag-overlay-path">{scope.currentPath}</span>
                            </p>
                        </div>
                    </div>
                )}

                <DriveHeader currentPath={scope.currentPath} trail={listing.listing?.trail} onNavigate={handleNavigate} />

                <DriveToolbar
                    currentScope={scope.currentScope}
                    checkAdmin={checkAdmin}
                    onSwitchScope={scope.switchScope}
                    viewMode={viewMode}
                    uploading={upload.uploading}
                    loading={listing.loading}
                    searchQuery={listing.searchQuery}
                    fileInputRef={upload.fileInputRef}
                    folderInputRef={upload.folderInputRef}
                    onSetViewMode={setViewMode}
                    onNewFolder={actions.openNewFolder}
                    onFileChange={upload.handleFileSelect}
                    onFolderChange={upload.handleFolderSelect}
                    onSearch={listing.setSearchQuery}
                    onRefresh={refresh}
                    capabilities={(listing.listing as any)?.capabilities}
                />

                {actions.showNewFolder && (
                    <NewFolderPrompt
                        inputRef={actions.newFolderInputRef}
                        name={actions.newFolderName}
                        creating={actions.creatingFolder}
                        onChange={actions.setNewFolderName}
                        onSubmit={actions.submitNewFolder}
                        onCancel={() => actions.setShowNewFolder(false)}
                    />
                )}

                <div className="drive-content-area">
                    <div className="drive-main-scroll">
                        {listing.loading ? (
                            <div className="drive-loading">
                                <Loader2 size={32} className="drive-spin" />
                                <span>Loading...</span>
                            </div>
                        ) : listing.listing ? (
                            <DriveContent
                                listing={listing.listing}
                                viewMode={viewMode}
                                currentPage={listing.currentPage}
                                debouncedSearch={listing.debouncedSearch}
                                isSelected={selection.isSelected}
                                onToggleSelect={selection.toggle}
                                onFolderClick={(folder) => scope.navigate(folder.path)}
                                onFolderDownload={actions.handleFolderDownload}
                                onFileClick={actions.handleFileClick}
                                onDownloadDecompressed={actions.handleFileDownloadDecompressed}
                                onPreview={(file) => setPreviewFile(file)}
                                onDelete={actions.handleDelete}
                                onRename={handleRenameOpen}
                                onInfo={actions.setInfoItem}
                                onPageChange={listing.setCurrentPage}
                                onMoveItems={actions.handleMoveItems}
                            />
                        ) : null}
                    </div>

                    {actions.infoItem && <DetailsPanel item={actions.infoItem} onClose={() => actions.setInfoItem(null)} />}
                </div>

                <SelectionBar
                    selectedCount={selection.selectedItems.length}
                    onClear={selection.clear}
                    onDelete={() => actions.handleBulkDelete(selection.selectedItems)}
                    onMove={() => showEvent("info", "To move items, drag and drop them into a folder.")}
                />

                <RenameModal
                    isOpen={!!actions.renamingItem}
                    initialName={actions.renamingItem?.name ?? ""}
                    onClose={() => actions.setRenamingItem(null)}
                    onConfirm={actions.handleRenameConfirm}
                />

                <FilePreviewModal
                    isOpen={!!previewFile}
                    file={previewFile}
                    currentScope={scope.currentScope}
                    onClose={() => setPreviewFile(null)}
                    onDownload={(file) => {
                        actions.handleFileClick(file);
                    }}
                />
            </div>
        </PageLayout>
    );
}
