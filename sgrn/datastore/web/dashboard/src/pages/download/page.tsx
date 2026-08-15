import { useState, useCallback, useEffect } from "react";
import { Loader2 } from "lucide-react";
import { PageLayout } from "@/components/PageLayout";
import { listFiles, downloadFile } from "@/backend/api/storage";
import { SearchPanel } from "@/pages/download/components/SearchPanel";
import { FilesTable } from "@/pages/download/components/FilesTable";
import { useEvent } from "@/contexts/EventContext";
import { handleSgrnResult } from "@/backend/errorHandler";
import type { FileMetadata, FilterType, ListFilesParams } from "@/pages/download/types";

export default function DownloadPage() {
    const { showEvent } = useEvent();

    // Filter state
    const [filterType, setFilterType] = useState<FilterType>("name");
    const [filterValue, setFilterValue] = useState("");

    // Pagination state
    const [limit, setLimit] = useState(50);
    const [offset, setOffset] = useState(0);

    // Data state
    const [fileMetadata, setFileMetadata] = useState<FileMetadata[] | null>(null);
    const [loading, setLoading] = useState(false);
    const [downloading, setDownloading] = useState<number | null>(null);
    const [totalCount, setTotalCount] = useState<number>(0);

    // User info
    const getUserInfo = () => {
        const userStr = sessionStorage.getItem("user_info") || localStorage.getItem("user_info");
        if (!userStr) return null;
        try {
            return JSON.parse(userStr);
        } catch {
            return null;
        }
    };

    const user = getUserInfo();
    const isAdmin = user?.role === "admin" || user?.role === "super_admin";

    /**
     * Build PostgREST query parameters from current state
     */
    const buildQueryParams = useCallback((): ListFilesParams => {
        let mode: ListFilesParams["mode"];
        let identifier: string | number | undefined;

        switch (filterType) {
            case "name":
                mode = "search";
                identifier = filterValue;
                break;
            case "extension":
                mode = "extension";
                identifier = filterValue;
                break;
            case "path":
                mode = "path";
                identifier = filterValue;
                break;
            case "domain":
                mode = "domain";
                identifier = isAdmin && filterValue ? filterValue : undefined;
                break;
            case "user":
                mode = "user";
                identifier = isAdmin && filterValue ? parseInt(filterValue) : undefined;
                break;
            case "session":
                mode = "session";
                identifier = filterValue || "current";
                break;
            default:
                mode = "search"; // Default case, perhaps for an empty filterType or unknown
                identifier = filterValue;
                break;
        }

        const params: ListFilesParams = {
            limit,
            offset,
            mode: mode,
            identifier: identifier,
        };

        return params;
    }, [filterType, filterValue, limit, offset, isAdmin]);

    /**
     * Fetch files from backend
     */
    const fetchFiles = useCallback(async () => {
        setLoading(true);

        try {
            const params = buildQueryParams();
            const result = await listFiles(params);

            if (handleSgrnResult(result)) {
                const data = result.data;
                setFileMetadata(data);
                setTotalCount(data.length); // PostgREST returns actual count in headers, but we use length for now
            } else {
                setFileMetadata([]);
            }
        } catch (error: unknown) {
            const errorMessage = error instanceof Error ? error.message : "Failed to fetch files";
            showEvent("error", errorMessage);
            setFileMetadata([]);
        } finally {
            setLoading(false);
        }
    }, [buildQueryParams, showEvent]);

    /**
     * Initial load and reload on pagination/filter changes
     */
    useEffect(() => {
        fetchFiles();
    }, [fetchFiles]);

    /**
     * Handle file download
     */
    const handleDownload = async (file: FileMetadata) => {
        setDownloading(file.id);
        try {
            // Direct API download (deprecated Minio direct download removed)
            const path = file.full_path || file.minio_key;
            const isCompressed = file.object?.is_compressed ?? false;
            const downResult = await downloadFile(path, file.name, isCompressed);

            if (handleSgrnResult(downResult)) {
                showEvent("success", `Downloaded ${file.name}`);
            }
        } catch (err: unknown) {
            const errorMessage = err instanceof Error ? err.message : "Download failed";
            showEvent("error", `Download failed: ${errorMessage}`);
        } finally {
            setDownloading(null);
        }
    };

    /**
     * Handle search - reset offset and fetch
     */
    const handleSearch = () => {
        setOffset(0);
        fetchFiles();
    };

    /**
     * Handle page change
     */
    const handleNextPage = () => {
        setOffset(offset + limit);
    };

    const handlePrevPage = () => {
        setOffset(Math.max(0, offset - limit));
    };

    const handleResetPage = () => {
        setOffset(0);
    };

    const filesArray = fileMetadata || [];
    const currentPage = Math.floor(offset / limit) + 1;
    const hasNextPage = filesArray.length === limit;
    const hasPrevPage = offset > 0;

    return (
        <PageLayout>
            <div className="download-page-container space-y-6">
                <div className="flex items-center justify-between">
                    <div>
                        <h1 className="text-3xl font-bold">File Browser</h1>
                        <p className="text-sm text-muted-foreground mt-1">{isAdmin ? "Browse all files (Admin)" : "Browse your files"}</p>
                    </div>
                </div>

                <SearchPanel
                    filterType={filterType}
                    filterValue={filterValue}
                    limit={limit}
                    offset={offset}
                    loading={loading}
                    isAdmin={isAdmin}
                    onFilterTypeChange={(type) => {
                        setFilterType(type);
                        setFilterValue("");
                        setOffset(0);
                    }}
                    onFilterValueChange={setFilterValue}
                    onLimitChange={(value) => {
                        setLimit(value);
                        setOffset(0);
                    }}
                    onSearch={handleSearch}
                    onNextPage={handleNextPage}
                    onPrevPage={handlePrevPage}
                    onResetPage={handleResetPage}
                />

                {/* Results Section */}
                {filesArray.length > 0 && (
                    <div className="space-y-4">
                        <div className="flex items-center justify-between">
                            <div className="space-y-1">
                                <h2 className="text-xl font-bold">Results ({filesArray.length})</h2>
                                <p className="text-xs text-muted-foreground">
                                    Page {currentPage} • Showing {offset + 1}-{offset + filesArray.length}
                                    {totalCount > 0 && ` of ${totalCount}`}
                                </p>
                            </div>

                            {/* Pagination Controls */}
                            <div className="flex items-center gap-2">
                                <span className="text-sm text-muted-foreground">Rows:</span>
                                <select
                                    value={limit}
                                    onChange={(e) => {
                                        setLimit(Number(e.target.value));
                                        setOffset(0);
                                    }}
                                    className="p-2 border rounded-md bg-background text-sm"
                                >
                                    <option value={10}>10</option>
                                    <option value={25}>25</option>
                                    <option value={50}>50</option>
                                    <option value={100}>100</option>
                                </select>
                            </div>
                        </div>

                        <FilesTable files={filesArray} downloading={downloading} onDownload={handleDownload} />

                        {/* Bottom Pagination */}
                        <div className="flex items-center justify-between">
                            <div className="flex gap-2">
                                <button
                                    onClick={handlePrevPage}
                                    disabled={!hasPrevPage}
                                    className="px-4 py-2 border rounded-md disabled:opacity-50 disabled:cursor-not-allowed hover:bg-muted"
                                >
                                    Previous
                                </button>
                                <button
                                    onClick={handleNextPage}
                                    disabled={!hasNextPage}
                                    className="px-4 py-2 border rounded-md disabled:opacity-50 disabled:cursor-not-allowed hover:bg-muted"
                                >
                                    Next
                                </button>
                            </div>
                            <button
                                onClick={handleResetPage}
                                disabled={offset === 0}
                                className="px-4 py-2 text-sm text-muted-foreground hover:text-foreground disabled:opacity-50"
                            >
                                Back to start
                            </button>
                        </div>
                    </div>
                )}

                {loading && (
                    <div className="download-page-loading flex flex-col items-center justify-center py-12">
                        <Loader2 className="w-8 h-8 animate-spin mb-4" />
                        <p className="text-sm text-muted-foreground">Loading files...</p>
                    </div>
                )}

                {!loading && filesArray.length === 0 && fileMetadata !== null && (
                    <div className="download-page-empty flex flex-col items-center justify-center py-12 border-2 border-dashed rounded-lg">
                        <p className="text-lg font-medium">No files found</p>
                        <p className="text-sm text-muted-foreground mt-2">
                            {filterValue ? "Try adjusting your search filters" : "Upload some files to get started"}
                        </p>
                    </div>
                )}
            </div>
        </PageLayout>
    );
}
