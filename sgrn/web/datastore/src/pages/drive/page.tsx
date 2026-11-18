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
//     DriveBar           – combined breadcrumb + scope/search/view/actions bar
//     NewFolderPrompt      – inline create-folder input
//     DriveContent         – grid/list + pagination
//     PaginationControls   – prev/next page row

import { useState, useCallback, useEffect, useRef } from "react";
import { Loader2, Upload } from "lucide-react";
import { PageLayout } from "@/components/PageLayout";
import { DriveBar } from "@/pages/drive/components/DriveBar";
import type { UploadNewVersionHandler } from "@/pages/drive/components/FileGrid";
import { DriveContent } from "@/pages/drive/components/DriveContent";
import { NewFolderPrompt } from "@/pages/drive/components/NewFolderPrompt";
import {
    listDirectory,
    downloadDriveFile,
    uploadDriveFile,
    uploadResumableDriveFile,
    uploadDriveFilesBatch,
    describeStorageTrace,
    summarizeBatchTrace,
    createDirectory,
    deleteDriveItem,
    renameItem,
    bulkAction,
    type StorageScope,
    type DriveSortKey,
    type DriveSortOrder,
} from "@/pages/drive/api";
import { toDisplayDrivePath } from "@/pages/drive/pathDisplay";
import { RenameModal } from "@/pages/drive/components/RenameModal";
import { DetailsPanel } from "@/pages/drive/components/DetailsPanel";
import { SelectionBar } from "@/pages/drive/components/SelectionBar";
import { FilePreviewModal } from "@/pages/drive/components/FilePreviewModal";
import { UploadProgressPanel, type UploadProgressItem } from "@/pages/drive/components/UploadProgressPanel";
import { useEvent } from "@/contexts/EventContext";
import { useAuth } from "@/contexts/AuthContext";
import type { DriveFolder, DriveFile } from "@sgrn/types";
import { isError } from "@sgrn/types";
import type { DriveListing, DriveMoveItem } from "@/pages/drive/types";
import { uploadDriveFileVersion } from "@/pages/drive/versioning";

// ─── helpers ────────────────────────────────────────────────────────────────

function getDriveItemKey(item: DriveMoveItem) {
    return `${item.type}:${item.id}`;
}

const PAGE_SIZE = 20;

let transferSequence = 0;
function createTransferId(): string {
    transferSequence += 1;
    return `transfer-${Date.now()}-${transferSequence}`;
}

function createEmptyListing(path: string): DriveListing {
    return {
        path,
        folders: [],
        files: [],
        total_items: 0,
        total_folders: 0,
        total_files: 0,
        page: 1,
        page_size: PAGE_SIZE,
        total_pages: 1,
    };
}

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
    const [listing, setListing] = useState<DriveListing | null>(null);
    const [loading, setLoading] = useState(false);
    const [currentPage, setCurrentPage] = useState(1);
    const [searchQuery, setSearchQuery] = useState("");
    const [debouncedSearch, setDebouncedSearch] = useState("");
    const [sortBy, setSortBy] = useState<DriveSortKey>("name");
    const [sortOrder, setSortOrder] = useState<DriveSortOrder>("asc");
    const [initialized, setInitialized] = useState(false);
    const requestGeneration = useRef(0);

    // reset page when path / scope / ordering change
    useEffect(() => {
        setCurrentPage(1);
    }, [currentPath, currentScope, sortBy, sortOrder]);

    // debounce search input
    useEffect(() => {
        const t = setTimeout(() => {
            setDebouncedSearch(searchQuery);
            setCurrentPage(1);
        }, 300);
        return () => clearTimeout(t);
    }, [searchQuery]);

    const fetchListing = useCallback(
        async (
            path = currentPath,
            scope = currentScope,
            page = currentPage,
            search = debouncedSearch,
            sort = sortBy,
            order = sortOrder,
        ) => {
            const generation = ++requestGeneration.current;
            setLoading(true);

            try {
                const result = await listDirectory(path, scope, page, PAGE_SIZE, search, sort, order);
                // Navigation/search can start another request before this one finishes.
                // Ignore stale responses so an older directory never replaces the active one.
                if (generation !== requestGeneration.current) return null;

                if (isError(result)) {
                    showEvent("error", result.error);
                    setListing(createEmptyListing(path));
                    return null;
                }

                setListing(result.data);
                return result.data;
            } catch (error: unknown) {
                if (generation === requestGeneration.current) {
                    const message = error instanceof Error ? error.message : String(error);
                    showEvent("error", `Failed to load directory: ${message}`);
                    setListing(createEmptyListing(path));
                }
                return null;
            } finally {
                if (generation === requestGeneration.current) setLoading(false);
            }
        },
        [currentPath, currentScope, currentPage, debouncedSearch, sortBy, sortOrder, showEvent],
    );

    // Invalidate outstanding requests when the Drive page unmounts.
    useEffect(
        () => () => {
            requestGeneration.current += 1;
        },
        [],
    );

    // initial + reactive fetch
    useEffect(() => {
        if (!initialized) return;
        fetchListing(currentPath, currentScope, currentPage, debouncedSearch, sortBy, sortOrder);
    }, [initialized, currentPath, currentScope, currentPage, debouncedSearch, sortBy, sortOrder, fetchListing]);

    /** Clicking an active column flips direction; a new column starts ascending. */
    const toggleSort = useCallback(
        (key: DriveSortKey) => {
            // Keep the updater pure: never call setSortOrder from inside the
            // setSortBy updater (StrictMode double-invokes updaters, which would
            // flip the direction twice and cancel the toggle out).
            if (key === sortBy) {
                setSortOrder((o) => (o === "asc" ? "desc" : "asc"));
            } else {
                setSortBy(key);
                setSortOrder("asc");
            }
        },
        [sortBy],
    );

    return {
        listing,
        loading,
        currentPage,
        searchQuery,
        debouncedSearch,
        sortBy,
        sortOrder,
        toggleSort,
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
    const [uploadItems, setUploadItems] = useState<UploadProgressItem[]>([]);
    // Encryption-at-rest preference persists across sessions so the default
    // upload mode (plaintext vs AES-256-GCM) survives a page reload.
    const [encryptUploads, setEncryptUploads] = useState<boolean>(() => {
        try {
            return localStorage.getItem("sgrn:encrypt-uploads") === "1";
        } catch {
            return false;
        }
    });
    const toggleEncrypt = useCallback(() => {
        setEncryptUploads((prev) => {
            const next = !prev;
            try {
                localStorage.setItem("sgrn:encrypt-uploads", next ? "1" : "0");
            } catch {
                // storage unavailable — the toggle stays session-local
            }
            return next;
        });
    }, []);
    const fileInputRef = useRef<HTMLInputElement>(null);
    const folderInputRef = useRef<HTMLInputElement>(null);

    const clearCompletedUploads = useCallback(() => {
        setUploadItems((prev) => prev.filter((i) => i.status !== "completed"));
    }, []);

    const updateItem = useCallback((id: string, patch: Partial<UploadProgressItem>) => {
        setUploadItems((prev) => prev.map((item) => (item.id === id ? { ...item, ...patch } : item)));
    }, []);

    const processSingleFile = useCallback(
        async (file: File) => {
            const id = createTransferId();
            const newItem: UploadProgressItem = {
                id,
                fileName: file.name,
                fileSize: file.size,
                status: "uploading",
                percent: 0,
                uploadedChunks: 0,
                totalChunks: Math.ceil(file.size / (5 * 1024 * 1024)) || 1,
            };

            setUploadItems((prev) => [newItem, ...prev]);

            try {
                if (file.size >= 5 * 1024 * 1024 || encryptUploads || currentScope === "domains") {
                    showEvent("info", `Initiating ${encryptUploads ? "encrypted " : ""}chunked upload for ${file.name}...`);
                    const result = await uploadResumableDriveFile(
                        currentPath,
                        file,
                        currentScope,
                        (progress) => {
                            updateItem(id, {
                                percent: progress.percent,
                                uploadedChunks: progress.uploadedChunks,
                                totalChunks: progress.totalChunks,
                                uploadedBytes: Math.round((file.size * progress.percent) / 100),
                            });
                        },
                        encryptUploads,
                    );
                    if (isError(result)) {
                        updateItem(id, { status: "error", errorMessage: result.error });
                        showEvent("error", `${file.name}: ${result.error}`);
                        return;
                    }

                    updateItem(id, { status: "completed", percent: 100, uploadedBytes: file.size });
                    // The datastore policy is authoritative: a domain may reject encryption.
                    const storedEncrypted = result.data?.is_encrypted ?? encryptUploads;
                    if (encryptUploads && !storedEncrypted) {
                        showEvent("info", `${file.name} uploaded (stored in plaintext: encryption is not allowed for this domain).`);
                    } else {
                        showEvent("success", `${file.name} uploaded successfully${storedEncrypted ? " (AES-256-GCM encrypted)" : ""}.`);
                    }
                    return;
                }

                const result = await uploadDriveFile(currentPath, file, currentScope);
                if (isError(result)) {
                    updateItem(id, { status: "error", errorMessage: result.error });
                    showEvent("error", `${file.name}: ${result.error}`);
                    return;
                }

                updateItem(id, { status: "completed", percent: 100, uploadedBytes: file.size });
                showEvent(
                    "success",
                    `${file.name} uploaded successfully${describeStorageTrace(result.data.upload_mode, result.data.part_count)}`,
                );
            } catch (error: unknown) {
                const message = error instanceof Error ? error.message : String(error);
                updateItem(id, { status: "error", errorMessage: message });
                showEvent("error", `${file.name}: ${message}`);
            }
        },
        [currentPath, currentScope, showEvent, updateItem, encryptUploads],
    );

    const processBatch = useCallback(
        async (files: File[], announceFolder = false): Promise<void> => {
            if (files.length === 0) return;
            const items: UploadProgressItem[] = files.map((file) => ({
                id: createTransferId(),
                fileName: file.name,
                fileSize: file.size,
                status: "uploading",
                percent: 0,
                uploadedChunks: 0,
                totalChunks: 1,
            }));
            setUploadItems((prev) => [...items, ...prev]);

            if (announceFolder) showEvent("info", `Uploading folder structure (${files.length} files)...`);

            try {
                const result = await uploadDriveFilesBatch(currentPath, files, currentScope);
                if (isError(result)) {
                    items.forEach((item) => updateItem(item.id, { status: "error", errorMessage: result.error }));
                    showEvent("error", result.error);
                    return;
                }

                const { success_count, fail_count, results } = result.data;
                items.forEach((item, index) => {
                    const outcome = results[index];
                    if (!outcome || outcome.success === false || outcome.error) {
                        updateItem(item.id, {
                            status: "error",
                            errorMessage: outcome?.error || "The datastore did not confirm this upload.",
                        });
                    } else {
                        updateItem(item.id, { status: "completed", percent: 100, uploadedBytes: item.fileSize });
                    }
                });

                const summary = announceFolder ? "Folder uploaded" : `${success_count} file${success_count === 1 ? "" : "s"} uploaded`;
                showEvent(
                    success_count > 0 ? "success" : "error",
                    success_count > 0
                        ? `${summary}${fail_count > 0 ? `, ${fail_count} failed` : ""}${summarizeBatchTrace(results)}`
                        : announceFolder
                          ? "Folder upload failed"
                          : `Failed to upload ${fail_count} files`,
                );
            } catch (error: unknown) {
                const message = error instanceof Error ? error.message : String(error);
                items.forEach((item) => updateItem(item.id, { status: "error", errorMessage: message }));
                showEvent("error", message);
            }
        },
        [currentPath, currentScope, showEvent, updateItem],
    );

    const handleFileSelect = useCallback(
        async (event: React.ChangeEvent<HTMLInputElement>) => {
            const input = event.currentTarget;
            const files = Array.from(input.files ?? []);
            if (files.length === 0) {
                input.value = "";
                return;
            }

            setUploading(true);
            try {
                if (files.length > 1) await processBatch(files);
                else await processSingleFile(files[0]);
            } finally {
                input.value = "";
                setUploading(false);
                onComplete();
            }
        },
        [onComplete, processBatch, processSingleFile],
    );

    const handleFolderSelect = useCallback(
        async (event: React.ChangeEvent<HTMLInputElement>) => {
            const input = event.currentTarget;
            const files = Array.from(input.files ?? []);
            if (files.length === 0) {
                input.value = "";
                return;
            }

            setUploading(true);
            try {
                await processBatch(files, true);
            } finally {
                input.value = "";
                setUploading(false);
                onComplete();
            }
        },
        [onComplete, processBatch],
    );

    const uploadFilesDirect = useCallback(
        async (files: File[]) => {
            if (files.length === 0) return;
            setUploading(true);
            try {
                if (files.length > 1) await processBatch(files);
                else await processSingleFile(files[0]);
            } finally {
                setUploading(false);
                onComplete();
            }
        },
        [onComplete, processBatch, processSingleFile],
    );

    const registerDownload = useCallback(
        async (file: DriveFile) => {
            const id = createTransferId();
            const newItem: UploadProgressItem = {
                id,
                fileName: file.name,
                fileSize: file.size || 0,
                status: "uploading",
                percent: 0,
                uploadedChunks: 1,
                totalChunks: 1,
                direction: "download",
            };

            setUploadItems((prev) => [newItem, ...prev]);
            showEvent("info", `Downloading ${file.name}...`);

            try {
                const result = await downloadDriveFile(file.path, file.name, currentScope, 0, (receivedBytes, totalBytes) => {
                    const total = totalBytes || file.size || receivedBytes || 1;
                    const percent = Math.min(100, Math.max(0, Math.round((receivedBytes / total) * 100)));
                    updateItem(id, { percent, uploadedBytes: receivedBytes, fileSize: total });
                });

                if (isError(result)) {
                    updateItem(id, { status: "error", errorMessage: result.error });
                    showEvent("error", result.error);
                    return;
                }

                updateItem(id, { status: "completed", percent: 100, uploadedBytes: file.size });
                showEvent("success", `${file.name} downloaded successfully`);
            } catch (error: unknown) {
                const message = error instanceof Error ? error.message : String(error);
                updateItem(id, { status: "error", errorMessage: message });
                showEvent("error", message);
            }
        },
        [currentScope, showEvent, updateItem],
    );

    return {
        uploading,
        fileInputRef,
        folderInputRef,
        handleFileSelect,
        handleFolderSelect,
        uploadFilesDirect,
        uploadItems,
        clearCompletedUploads,
        registerDownload,
        encryptUploads,
        toggleEncrypt,
    };
}

function useDriveActions(
    currentPath: string,
    currentScope: StorageScope,
    onRefresh: () => void,
    onClearSelection: () => void,
    showEvent: ReturnType<typeof useEvent>["showEvent"],
    listing?: DriveListing | null,
    onDownloadFile?: (file: DriveFile) => void | Promise<void>,
    encryptUploads: boolean = false,
) {
    const [showNewFolder, setShowNewFolder] = useState(false);
    const [newFolderName, setNewFolderName] = useState("");
    const [creatingFolder, setCreatingFolder] = useState(false);
    const [renamingItem, setRenamingItem] = useState<{ id: number; type: "file" | "folder"; name: string } | null>(null);
    const [infoItem, setInfoItem] = useState<DriveFolder | DriveFile | null>(null);
    const newFolderInputRef = useRef<HTMLInputElement>(null);

    // Auto-focus the field without leaving a timer alive after the prompt closes.
    useEffect(() => {
        if (!showNewFolder) return;
        const timer = window.setTimeout(() => newFolderInputRef.current?.focus(), 50);
        return () => window.clearTimeout(timer);
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
        try {
            const result = await createDirectory(fullPath, currentScope);
            if (isError(result)) {
                showEvent("error", result.error);
                return;
            }

            showEvent("success", `Folder "${name}" created`);
            setShowNewFolder(false);
            setNewFolderName("");
            onRefresh();
            onClearSelection();
        } catch (error: unknown) {
            showEvent("error", `Failed to create folder: ${error instanceof Error ? error.message : String(error)}`);
        } finally {
            setCreatingFolder(false);
        }
    }, [newFolderName, currentPath, currentScope, showEvent, onRefresh, onClearSelection, listing]);

    const handleDelete = useCallback(
        async (item: DriveFolder | DriveFile, type: "file" | "folder") => {
            const canDelete = listing?.capabilities?.can_delete !== false;
            if (!canDelete) {
                showEvent("error", "Delete permission denied in this workspace");
                return;
            }

            const id = item.id;
            if (id == null) {
                showEvent("error", "Cannot delete virtual folders");
                return;
            }
            if (!confirm(`Are you sure you want to delete this ${type}?`)) return;

            showEvent("info", `Deleting ${item.name}...`);
            const result = await deleteDriveItem(id, type, currentScope);
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
                showEvent("success", `Processed ${result.data.success_count} of ${result.data.total} items`);
                onClearSelection();
                onRefresh();
            }
        },
        [showEvent, onRefresh, onClearSelection, listing],
    );

    const handleRenameConfirm = useCallback(
        async (newName: string): Promise<boolean> => {
            const canWrite = listing?.capabilities?.can_write !== false;
            if (!canWrite) {
                showEvent("error", "Rename permission denied in this workspace");
                return false;
            }

            if (!renamingItem) return false;
            const result = await renameItem(renamingItem.id, renamingItem.type, newName);
            if (isError(result)) {
                showEvent("error", result.error);
                return false;
            }
            showEvent("success", "Item renamed successfully");
            onRefresh();
            return true;
        },
        [renamingItem, showEvent, onRefresh, listing],
    );

    const handleFileClick = useCallback(
        async (file: DriveFile) => {
            try {
                if (onDownloadFile) {
                    await onDownloadFile(file);
                    return;
                }

                showEvent("info", `Downloading ${file.name}...`);
                const result = await downloadDriveFile(file.path, file.name, currentScope);
                if (isError(result)) showEvent("error", result.error);
                else showEvent("success", `${file.name} downloaded successfully`);
            } catch (error: unknown) {
                showEvent("error", `Download failed: ${error instanceof Error ? error.message : String(error)}`);
            }
        },
        [showEvent, currentScope, onDownloadFile],
    );

    const handleFileDownloadDecompressed = useCallback(
        async (file: DriveFile) => {
            showEvent("info", `Decompressing & downloading ${file.name}...`);
            try {
                const { fetchDriveFileContent } = await import("@/pages/drive/api");
                const result = await fetchDriveFileContent(file.path, currentScope);
                if (isError(result)) {
                    showEvent("error", result.error);
                    return;
                }

                const view = new Uint8Array(result.data);
                const { decompress } = await import("fzstd");
                const decompressed = decompress(view).slice();
                const blob = new Blob([decompressed.buffer as ArrayBuffer], { type: "application/octet-stream" });
                const url = URL.createObjectURL(blob);
                const anchor = document.createElement("a");
                anchor.href = url;
                const newName = file.name.toLowerCase().endsWith(".zst") ? file.name.slice(0, -4) : file.name;
                anchor.download = newName;
                document.body.appendChild(anchor);
                anchor.click();
                document.body.removeChild(anchor);
                window.setTimeout(() => URL.revokeObjectURL(url), 1000);
                showEvent("success", `${newName} decompressed successfully`);
            } catch (error: unknown) {
                showEvent("error", `Failed to decompress ${file.name}: ${error instanceof Error ? error.message : String(error)}`);
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

                type ZipFolder = InstanceType<typeof import("jszip")>;
                const processFolder = async (path: string, currentZipFolder: ZipFolder): Promise<void> => {
                    let page = 1;
                    let hasMore = true;
                    while (hasMore) {
                        const result = await listDirectory(path, currentScope, page, 100);
                        if (isError(result)) throw new Error(`Failed to list ${path}: ${result.error}`);
                        const directoryListing = result.data;

                        // Process files
                        for (const file of directoryListing.files) {
                            showEvent("info", `Downloading ${file.name}...`);

                            const fileRes = await fetchDriveFileContent(file.path, currentScope);
                            if (isError(fileRes)) throw new Error(`Failed to download ${file.path}: ${fileRes.error}`);
                            {
                                const buffer = fileRes.data;
                                const view = new Uint8Array(buffer);

                                let isZstd = file.name.toLowerCase().endsWith(".zst");
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
                                        const decompressed = decompress(view).slice();
                                        dataToSave = decompressed;
                                        if (fileName.toLowerCase().endsWith(".zst")) {
                                            fileName = fileName.slice(0, -4);
                                        }
                                    } catch (error: unknown) {
                                        // Preserve the original compressed file rather than writing corrupt bytes under an uncompressed name.
                                        showEvent(
                                            "info",
                                            `Could not decompress ${file.name}; adding the original .zst file to the archive.`,
                                        );
                                    }
                                }

                                currentZipFolder.file(fileName, dataToSave);
                                downloadedFiles++;
                            }
                        }

                        // Process subfolders sequentially to avoid swamping the server
                        for (const sub of directoryListing.folders) {
                            if (sub.token) continue; // Skip virtual infrastructure folders if they exist
                            const subFolderZip = currentZipFolder.folder(sub.name);
                            if (subFolderZip) await processFolder(sub.path, subFolderZip);
                        }

                        hasMore = page < directoryListing.total_pages;
                        page++;
                    }
                };

                const rootZipFolder = zip.folder(folder.name);
                if (!rootZipFolder) throw new Error("Unable to create the ZIP root folder.");
                await processFolder(folder.path, rootZipFolder);

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
                window.setTimeout(() => URL.revokeObjectURL(url), 1000);
                showEvent("success", `Downloaded ${folder.name}.zip`);
            } catch (err: unknown) {
                showEvent("error", `Error creating zip: ${err instanceof Error ? err.message : String(err)}`);
            }
        },
        [showEvent, currentScope],
    );

    const handleMoveItems = useCallback(
        async (items: DriveMoveItem[], targetId: number) => {
            if (!items.length) return;
            if (listing?.capabilities?.can_write === false) {
                showEvent("error", "Write permission denied in this workspace");
                return;
            }
            if (!Number.isSafeInteger(targetId) || targetId <= 0) {
                showEvent("error", "The target folder is invalid");
                return;
            }
            showEvent("info", `Moving ${items.length} item${items.length > 1 ? "s" : ""}...`);

            const result = await bulkAction(
                "move",
                items.map((i) => ({ id: i.id, type: i.type })),
                targetId,
            );

            if (isError(result)) {
                showEvent("error", result.error);
            } else {
                showEvent("success", `Moved ${result.data.success_count} of ${result.data.total} items`);
                onRefresh();
                onClearSelection();
            }
        },
        [showEvent, onRefresh, onClearSelection, listing],
    );

    const handleUploadNewVersion = useCallback<UploadNewVersionHandler>(
        async (existingFile, replacementFile, onProgress) => {
            const canWrite = listing?.capabilities?.can_write !== false;
            if (!canWrite) {
                const message = "Write permission denied in this workspace";
                showEvent("error", message);
                throw new Error(message);
            }

            showEvent("info", `Uploading a new version of ${existingFile.name}...`);
            const result = await uploadDriveFileVersion(existingFile, replacementFile, currentScope, onProgress, encryptUploads);
            if (isError(result)) {
                showEvent("error", result.error);
                throw new Error(result.error);
            }

            const encrypted = result.data.is_encrypted ?? (encryptUploads || existingFile.is_encrypted);
            showEvent(
                "success",
                `New version of ${existingFile.name} uploaded successfully${encrypted ? " (AES-256-GCM encrypted)" : ""}.`,
            );
            onRefresh();
        },
        [currentScope, encryptUploads, listing, onRefresh, showEvent],
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
        handleUploadNewVersion,
    };
}

// ─── local components ────────────────────────────────────────────────────────

// ─── page ────────────────────────────────────────────────────────────────────

export default function DrivePage() {
    const { showEvent } = useEvent();
    const { user } = useAuth();

    const [viewMode, setViewMode] = useState<"grid" | "list">("grid");

    const role = user?.role;
    const checkAdmin = role === "admin" || (typeof role === "object" && role !== null && "name" in role && role.name === "admin");
    const [previewFile, setPreviewFile] = useState<DriveFile | null>(null);

    // hooks
    const scope = useDriveScope(checkAdmin);
    const listing = useDriveListing(scope.currentPath, scope.currentScope, showEvent);
    const selection = useDriveSelection();

    const refresh = useCallback(() => {
        void listing.fetchListing(scope.currentPath, scope.currentScope, listing.currentPage, listing.debouncedSearch);
    }, [listing.fetchListing, listing.currentPage, listing.debouncedSearch, scope.currentPath, scope.currentScope]);

    const upload = useDriveUpload(
        scope.currentPath,
        scope.currentScope,
        () => {
            refresh();
            selection.clear();
        },
        showEvent,
    );

    const actions = useDriveActions(
        scope.currentPath,
        scope.currentScope,
        refresh,
        selection.clear,
        showEvent,
        listing.listing,
        upload.registerDownload,
        upload.encryptUploads,
    );

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
        [upload.uploadFilesDirect],
    );

    const handleNavigate = useCallback(
        (path: string) => {
            scope.navigate(path);
            selection.clear();
            actions.setInfoItem(null);
        },
        [scope.navigate, selection.clear, actions.setInfoItem],
    );

    // Stable identities so the memoized DriveContent/FileList can skip
    // re-rendering when only transient state (e.g. upload progress) changes.
    const handleFolderClick = useCallback((folder: DriveFolder) => scope.navigate(folder.path), [scope.navigate]);
    const handlePreview = useCallback((file: DriveFile) => setPreviewFile(file), []);

    const handleRenameOpen = useCallback(
        (item: DriveFolder | DriveFile, type: "file" | "folder") => {
            if (item.id == null) {
                showEvent("error", "This virtual item cannot be renamed.");
                return;
            }
            actions.setRenamingItem({ id: item.id, type, name: item.name });
        },
        [actions.setRenamingItem, showEvent],
    );

    return (
        <PageLayout>
            <div className="drive-page" onDragOver={handleDragOver}>
                {isDragging && (
                    <div className="drive-drag-overlay" onDragOver={handleDragOver} onDragLeave={handleDragLeave} onDrop={handleDrop}>
                        <div className="drive-drag-overlay-card">
                            <Upload size={40} className="drive-drag-overlay-icon" />
                            <h3 className="drive-drag-overlay-title">Drop files to upload</h3>
                            <p className="drive-drag-overlay-subtitle">
                                Your files will be preserved within{" "}
                                <span className="drive-drag-overlay-path">{toDisplayDrivePath(scope.currentPath)}</span>
                            </p>
                        </div>
                    </div>
                )}

                <DriveBar
                    currentPath={scope.currentPath}
                    trail={listing.listing?.trail}
                    onNavigate={handleNavigate}
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
                    sortBy={listing.sortBy}
                    sortOrder={listing.sortOrder}
                    onSort={listing.toggleSort}
                    capabilities={listing.listing?.capabilities}
                    encryptUploads={upload.encryptUploads}
                    onToggleEncrypt={upload.toggleEncrypt}
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
                                sortBy={listing.sortBy}
                                sortOrder={listing.sortOrder}
                                onSort={listing.toggleSort}
                                isSelected={selection.isSelected}
                                onToggleSelect={selection.toggle}
                                onFolderClick={handleFolderClick}
                                onFolderDownload={actions.handleFolderDownload}
                                onFileClick={actions.handleFileClick}
                                onDownloadDecompressed={actions.handleFileDownloadDecompressed}
                                onPreview={handlePreview}
                                onDelete={actions.handleDelete}
                                onRename={handleRenameOpen}
                                onInfo={actions.setInfoItem}
                                onPageChange={listing.setCurrentPage}
                                onMoveItems={actions.handleMoveItems}
                                onUploadNewVersion={actions.handleUploadNewVersion}
                            />
                        ) : null}
                    </div>

                    {actions.infoItem && (
                        <DetailsPanel
                            item={actions.infoItem}
                            scope={scope.currentScope}
                            canWrite={listing.listing?.capabilities?.can_write !== false}
                            canDelete={listing.listing?.capabilities?.can_delete !== false}
                            onClose={() => actions.setInfoItem(null)}
                            onChanged={() => listing.fetchListing()}
                        />
                    )}
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

                <UploadProgressPanel items={upload.uploadItems} onClearCompleted={upload.clearCompletedUploads} />

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
