// @/pages/drive/components/DriveBar.tsx
//
// Combined drive header + toolbar (formerly DriveHeader and DriveToolbar).
// One .drive-toolbar-row bar (scope tabs, view toggle, stretch search,
// actions) with the breadcrumb on its own line underneath.
//
// Layout of this file:
//   ScopeTabs      – Personal / Domains for everyone, Services / Users for admins
//   ViewToggle     – grid / list
//   SearchBox      – listing search
//   SortMenu       – sort key dropdown
//   UploadButtons  – new folder, upload folder / files (+ hidden inputs)
//   EncryptToggle  – encryption-at-rest default for uploads
//   DriveBar       – composes the pieces above

import type { InputHTMLAttributes, RefObject, ChangeEvent } from "react";
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
import type { DriveCapabilities } from "@/pages/drive/types";
import { Breadcrumb } from "@/pages/drive/components/Breadcrumb";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";

const DIRECTORY_INPUT_ATTRIBUTES: InputHTMLAttributes<HTMLInputElement> & { webkitdirectory: string } = {
    webkitdirectory: "",
};

const SORT_LABELS: Record<DriveSortKey, string> = {
    name: "Name",
    size: "Size",
    modified: "Modified",
};

type ViewMode = "grid" | "list";

interface ScopeTabConfig {
    scope: StorageScope;
    label: string;
    adminOnly: boolean;
}

/**
 * Domains are visible to every signed-in user (the datastore decides which
 * domains they can actually open); services and the users namespace stay
 * admin-only.
 */
const SCOPE_TABS: readonly ScopeTabConfig[] = [
    { scope: "personal", label: "Personal", adminOnly: false },
    { scope: "automated_services", label: "Services", adminOnly: true },
    { scope: "users", label: "Users", adminOnly: true },
    { scope: "domains", label: "Domains", adminOnly: false },
];

const WRITE_DENIED_TITLE = "Write permission denied in this workspace";

// ─── pieces ──────────────────────────────────────────────────────────────────

interface ScopeTabsProps {
    currentScope: StorageScope;
    isAdmin: boolean;
    onSwitchScope: (scope: StorageScope) => void;
}

function ScopeTabs({ currentScope, isAdmin, onSwitchScope }: ScopeTabsProps) {
    const visibleTabs = SCOPE_TABS.filter((tab) => isAdmin || !tab.adminOnly);

    return (
        <div className="drive-scope-tabs">
            {visibleTabs.map(({ scope, label }) => (
                <button
                    key={scope}
                    type="button"
                    onClick={() => onSwitchScope(scope)}
                    className={`drive-scope-btn ${currentScope === scope ? "drive-scope-btn-active" : ""}`}
                >
                    {scope !== "personal" && <Shield size={14} />}
                    <span>{label}</span>
                </button>
            ))}
        </div>
    );
}

function ViewToggle({ viewMode, onSetViewMode }: { viewMode: ViewMode; onSetViewMode: (mode: ViewMode) => void }) {
    const options: { mode: ViewMode; title: string; icon: typeof LayoutGrid }[] = [
        { mode: "grid", title: "Grid view", icon: LayoutGrid },
        { mode: "list", title: "List view", icon: List },
    ];

    return (
        <div className="drive-view-toggle" role="group" aria-label="View mode">
            {options.map(({ mode, title, icon: Icon }) => (
                <button
                    key={mode}
                    type="button"
                    onClick={() => onSetViewMode(mode)}
                    className={`drive-view-btn${viewMode === mode ? " drive-view-btn-active" : ""}`}
                    title={title}
                    aria-pressed={viewMode === mode}
                >
                    <Icon size={16} />
                </button>
            ))}
        </div>
    );
}

function SearchBox({ value, onChange }: { value: string; onChange: (query: string) => void }) {
    return (
        <div className="drive-search-wrapper">
            <Search size={16} className="drive-search-icon" />
            <input
                type="search"
                placeholder="Search files and folders"
                className="drive-search-input"
                value={value}
                onChange={(e) => onChange(e.target.value)}
                aria-label="Search files and folders"
            />
        </div>
    );
}

interface SortMenuProps {
    sortBy: DriveSortKey;
    sortOrder: DriveSortOrder;
    onSort: (key: DriveSortKey) => void;
}

function SortMenu({ sortBy, sortOrder, onSort }: SortMenuProps) {
    return (
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
    );
}

interface UploadButtonsProps {
    canWrite: boolean;
    uploading: boolean;
    fileInputRef: RefObject<HTMLInputElement | null>;
    folderInputRef: RefObject<HTMLInputElement | null>;
    onNewFolder: () => void;
    onFileChange: (e: ChangeEvent<HTMLInputElement>) => void;
    onFolderChange: (e: ChangeEvent<HTMLInputElement>) => void;
}

function UploadButtons({
    canWrite,
    uploading,
    fileInputRef,
    folderInputRef,
    onNewFolder,
    onFileChange,
    onFolderChange,
}: UploadButtonsProps) {
    return (
        <>
            <button
                type="button"
                onClick={onNewFolder}
                disabled={!canWrite}
                className="btn-industrial-secondary drive-toolbar-btn"
                title={canWrite ? "Create folder" : WRITE_DENIED_TITLE}
            >
                <FolderPlus size={16} />
                <span>New folder</span>
            </button>

            <button
                type="button"
                onClick={() => folderInputRef.current?.click()}
                disabled={uploading || !canWrite}
                className="btn-industrial-secondary drive-toolbar-btn"
                title={canWrite ? "Upload folder" : WRITE_DENIED_TITLE}
            >
                <Folder size={16} />
                <span>Upload folder</span>
            </button>
            <input
                ref={folderInputRef}
                type="file"
                {...DIRECTORY_INPUT_ATTRIBUTES}
                className="drive-hidden-input"
                onChange={onFolderChange}
            />

            <button
                type="button"
                onClick={() => fileInputRef.current?.click()}
                disabled={uploading || !canWrite}
                className="btn-industrial-primary drive-toolbar-btn"
                title={canWrite ? "Upload files" : WRITE_DENIED_TITLE}
            >
                <Upload size={16} />
                <span>Upload files</span>
            </button>
            <input ref={fileInputRef} type="file" multiple className="drive-hidden-input" onChange={onFileChange} />
        </>
    );
}

function EncryptToggle({ enabled, onToggle }: { enabled: boolean; onToggle: () => void }) {
    return (
        <button
            type="button"
            onClick={onToggle}
            className={`btn-industrial-secondary drive-toolbar-btn ${enabled ? "text-amber-500 border-amber-500/40 bg-amber-500/10" : ""}`}
            title={enabled ? "Encryption-at-rest: ON (AES-256-GCM)" : "Encryption-at-rest: OFF (Plaintext)"}
            aria-label="Toggle upload encryption"
            aria-pressed={enabled}
        >
            <Lock size={15} />
            <span>{enabled ? "Encrypted" : "Plaintext"}</span>
        </button>
    );
}

// ─── bar ─────────────────────────────────────────────────────────────────────

interface DriveBarProps {
    currentPath: string;
    trail: DirectoryListing["trail"];
    onNavigate: (path: string) => void;
    currentScope: StorageScope;
    isAdmin: boolean;
    onSwitchScope: (scope: StorageScope) => void;
    viewMode: ViewMode;
    uploading: boolean;
    loading: boolean;
    searchQuery: string;
    fileInputRef: RefObject<HTMLInputElement | null>;
    folderInputRef: RefObject<HTMLInputElement | null>;
    onSetViewMode: (mode: ViewMode) => void;
    onNewFolder: () => void;
    onFileChange: (e: ChangeEvent<HTMLInputElement>) => void;
    onFolderChange: (e: ChangeEvent<HTMLInputElement>) => void;
    onSearch: (q: string) => void;
    onRefresh: () => void;
    sortBy: DriveSortKey;
    sortOrder: DriveSortOrder;
    onSort: (key: DriveSortKey) => void;
    capabilities?: DriveCapabilities;
    encryptUploads?: boolean;
    onToggleEncrypt?: () => void;
}

export function DriveBar({
    currentPath,
    trail,
    onNavigate,
    currentScope,
    isAdmin,
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
            <div className="drive-toolbar-row">
                <ScopeTabs currentScope={currentScope} isAdmin={isAdmin} onSwitchScope={onSwitchScope} />
                <ViewToggle viewMode={viewMode} onSetViewMode={onSetViewMode} />
                <SearchBox value={searchQuery} onChange={onSearch} />

                <div className="drive-toolbar-actions">
                    <SortMenu sortBy={sortBy} sortOrder={sortOrder} onSort={onSort} />
                    <UploadButtons
                        canWrite={canWrite}
                        uploading={uploading}
                        fileInputRef={fileInputRef}
                        folderInputRef={folderInputRef}
                        onNewFolder={onNewFolder}
                        onFileChange={onFileChange}
                        onFolderChange={onFolderChange}
                    />
                    {onToggleEncrypt && <EncryptToggle enabled={encryptUploads} onToggle={onToggleEncrypt} />}
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

            <div className="drive-breadcrumb-row">
                <Breadcrumb path={currentPath} trail={trail} onNavigate={onNavigate} />
            </div>
        </div>
    );
}
