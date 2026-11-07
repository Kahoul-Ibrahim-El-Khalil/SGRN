// @/pages/drive/components/DriveBar.tsx
//
// Combined drive header + toolbar (formerly DriveHeader and DriveToolbar).
// One .drive-toolbar-row bar (scope tabs, view toggle, stretch search,
// actions) with the breadcrumb on its own line underneath.

import {
    Shield,
    LayoutGrid,
    List,
    Search,
    Folder,
    FolderPlus,
    Upload,
    RefreshCw,
    ArrowUpDown,
    ArrowUp,
    ArrowDown,
    Check,
    Lock,
} from "lucide-react";
import type { StorageScope, DriveSortKey, DriveSortOrder } from "@/pages/drive/api";
import type { DirectoryListing } from "@sgrn/types";
import { Breadcrumb } from "@/pages/drive/components/Breadcrumb";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";

const SORT_LABELS: Record<DriveSortKey, string> = {
    name: "Name",
    size: "Size",
    modified: "Modified",
};

interface DriveBarProps {
    currentPath: string;
    trail: DirectoryListing["trail"];
    onNavigate: (path: string) => void;
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
    sortBy: DriveSortKey;
    sortOrder: DriveSortOrder;
    onSort: (key: DriveSortKey) => void;
    capabilities?: any;
    encryptUploads?: boolean;
    onToggleEncrypt?: () => void;
}

export function DriveBar({
    currentPath,
    trail,
    onNavigate,
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
    sortBy,
    sortOrder,
    onSort,
    capabilities,
    encryptUploads = false,
    onToggleEncrypt,
}: DriveBarProps) {
    const canWrite = capabilities?.can_write !== false;

    return (
        <div className="drive-bar">
            {/* Single toolbar bar */}
            <div className="drive-toolbar-row">
                <div className="drive-scope-tabs">
                    {(["personal", ...(checkAdmin ? ["automated_services", "users", "domains"] : [])] as StorageScope[]).map((scope) => (
                        <button
                            key={scope}
                            type="button"
                            onClick={() => onSwitchScope(scope)}
                            className={`drive-scope-btn ${currentScope === scope ? "drive-scope-btn-active" : ""}`}
                        >
                            {scope !== "personal" && <Shield size={14} />}
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

                <div className="drive-view-toggle" role="group" aria-label="View mode">
                    <button
                        type="button"
                        onClick={() => onSetViewMode("grid")}
                        className={`drive-view-btn${viewMode === "grid" ? " drive-view-btn-active" : ""}`}
                        title="Grid view"
                        aria-pressed={viewMode === "grid"}
                    >
                        <LayoutGrid size={16} />
                    </button>
                    <button
                        type="button"
                        onClick={() => onSetViewMode("list")}
                        className={`drive-view-btn${viewMode === "list" ? " drive-view-btn-active" : ""}`}
                        title="List view"
                        aria-pressed={viewMode === "list"}
                    >
                        <List size={16} />
                    </button>
                </div>

                <div className="drive-search-wrapper">
                    <Search size={16} className="drive-search-icon" />
                    <input
                        type="search"
                        placeholder="Search files and folders"
                        className="drive-search-input"
                        value={searchQuery}
                        onChange={(e) => onSearch(e.target.value)}
                        aria-label="Search files and folders"
                    />
                </div>

                <div className="drive-toolbar-actions">
                    <DropdownMenu>
                        <DropdownMenuTrigger asChild>
                            <button
                                type="button"
                                className="btn-industrial-secondary drive-toolbar-btn"
                                title="Sort listing"
                                aria-label={`Sorted by ${SORT_LABELS[sortBy]}, ${sortOrder === "asc" ? "ascending" : "descending"}`}
                            >
                                <ArrowUpDown size={16} />
                                <span>{SORT_LABELS[sortBy]}</span>
                                {sortOrder === "asc" ? <ArrowUp size={14} /> : <ArrowDown size={14} />}
                            </button>
                        </DropdownMenuTrigger>
                        <DropdownMenuContent align="start" className="w-44 bg-popover border-border text-popover-foreground shadow-xl">
                            {(Object.keys(SORT_LABELS) as DriveSortKey[]).map((key) => (
                                <DropdownMenuItem
                                    key={key}
                                    className="cursor-pointer hover:bg-foreground/5 focus:bg-foreground/5 py-2"
                                    onClick={() => onSort(key)}
                                >
                                    <span className="flex-1">{SORT_LABELS[key]}</span>
                                    {sortBy === key && <Check size={14} className="text-primary" />}
                                </DropdownMenuItem>
                            ))}
                        </DropdownMenuContent>
                    </DropdownMenu>

                    <button
                        type="button"
                        onClick={onNewFolder}
                        disabled={!canWrite}
                        className="btn-industrial-secondary drive-toolbar-btn"
                        title={!canWrite ? "Write permission denied in this workspace" : "Create folder"}
                    >
                        <FolderPlus size={16} />
                        <span>New folder</span>
                    </button>

                    <button
                        type="button"
                        onClick={() => folderInputRef.current?.click()}
                        disabled={uploading || !canWrite}
                        className="btn-industrial-secondary drive-toolbar-btn"
                        title={!canWrite ? "Write permission denied in this workspace" : "Upload folder"}
                    >
                        <Folder size={16} />
                        <span>Upload folder</span>
                    </button>
                    <input
                        ref={folderInputRef}
                        type="file"
                        {...({ webkitdirectory: "" } as any)}
                        style={{ display: "none" }}
                        onChange={onFolderChange}
                    />

                    <button
                        type="button"
                        onClick={() => fileInputRef.current?.click()}
                        disabled={uploading || !canWrite}
                        className="btn-industrial-primary drive-toolbar-btn"
                        title={!canWrite ? "Write permission denied in this workspace" : "Upload files"}
                    >
                        <Upload size={16} />
                        <span>Upload files</span>
                    </button>
                    <input ref={fileInputRef} type="file" multiple style={{ display: "none" }} onChange={onFileChange} />

                    {onToggleEncrypt && (
                        <button
                            type="button"
                            onClick={onToggleEncrypt}
                            className={`btn-industrial-secondary drive-toolbar-btn ${encryptUploads ? "text-amber-500 border-amber-500/40 bg-amber-500/10" : ""}`}
                            title={encryptUploads ? "Encryption-at-rest: ON (AES-256-GCM)" : "Encryption-at-rest: OFF (Plaintext)"}
                            aria-label="Toggle upload encryption"
                        >
                            <Lock size={15} />
                            <span>{encryptUploads ? "Encrypted" : "Plaintext"}</span>
                        </button>
                    )}

                    <button
                        type="button"
                        onClick={onRefresh}
                        className="btn-industrial-secondary drive-toolbar-btn drive-refresh-btn"
                        title="Refresh"
                        aria-label="Refresh listing"
                    >
                        <RefreshCw size={16} className={loading ? "drive-spin" : ""} />
                    </button>
                </div>
            </div>

            {/* Breadcrumb on its own line underneath the toolbar */}
            <div className="drive-breadcrumb-row">
                <Breadcrumb path={currentPath} trail={trail} onNavigate={onNavigate} />
            </div>
        </div>
    );
}
