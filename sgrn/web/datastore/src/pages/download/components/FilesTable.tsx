// @/components/workspace/download/FilesTable.tsx
import { useState } from "react";
import { FileText, File, ChevronUp, ChevronDown, Download, Loader2, Calendar, FolderOpen } from "lucide-react";

import type { FileMetadata } from "@/pages/download/types";
import { SizeDisplay } from "./FileSizeDisplay";
import { Button } from "@/components/ui/button";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import { Badge } from "@/components/ui/badge";

interface FilesTableProps {
    files: FileMetadata[];
    downloading: number | null;
    onDownload: (file: FileMetadata) => void;
}

type SortField = keyof FileMetadata;
type SortDirection = "asc" | "desc";

export function FilesTable({ files, downloading, onDownload }: FilesTableProps) {
    const [sortField, setSortField] = useState<SortField>("created_at");
    const [sortDirection, setSortDirection] = useState<SortDirection>("desc");

    const handleSort = (field: SortField) => {
        if (sortField === field) {
            setSortDirection(sortDirection === "asc" ? "desc" : "asc");
        } else {
            setSortField(field);
            setSortDirection("desc"); // Default to newest first for new fields
        }
    };

    const sortedFiles = [...files].sort((a, b) => {
        const aValue = a[sortField];
        const bValue = b[sortField];

        if (aValue === bValue) return 0;

        // Handle string vs number comparisons safely
        const compareResult = (aValue ?? "") > (bValue ?? "") ? 1 : -1;
        return sortDirection === "asc" ? compareResult : -compareResult;
    });

    const formatDateTime = (isoString: string): { date: string; time: string } => {
        const date = new Date(isoString);
        return {
            date: date.toLocaleDateString("en-US", { month: "short", day: "numeric", year: "numeric" }),
            time: date.toLocaleTimeString("en-US", { hour: "2-digit", minute: "2-digit" }),
        };
    };

    const getRelativeTime = (isoString: string): string => {
        const now = new Date();
        const date = new Date(isoString);
        const diffMs = now.getTime() - date.getTime();
        const diffMins = Math.floor(diffMs / 60000);
        const diffHours = Math.floor(diffMs / 3600000);
        const diffDays = Math.floor(diffMs / 86400000);

        if (diffMins < 1) return "Just now";
        if (diffMins < 60) return `${diffMins}m ago`;
        if (diffHours < 24) return `${diffHours}h ago`;
        if (diffDays < 7) return `${diffDays}d ago`;
        return formatDateTime(isoString).date;
    };

    const getStatusBadge = (isCompressed: boolean | undefined) => {
        // Files are always "ready" in the new schema (status field removed)
        // Show compression status instead if applicable
        if (isCompressed) {
            return (
                <Badge variant="secondary" className="text-[10px] px-1.5 py-0.5">
                    Compressed
                </Badge>
            );
        }
        return null;
    };

    return (
        <div className="rounded-md border bg-card">
            <div className="w-full overflow-auto max-h-[600px]">
                <Table>
                    <TableHeader className="bg-muted/50 sticky top-0 z-10">
                        <TableRow>
                            <TableHead className="w-[50px] text-center">Type</TableHead>
                            <TableHead className="cursor-pointer hover:bg-muted/80 transition-colors" onClick={() => handleSort("name")}>
                                <div className="flex items-center gap-2">
                                    File Details
                                    {sortField === "name" &&
                                        (sortDirection === "asc" ? <ChevronUp size={14} /> : <ChevronDown size={14} />)}
                                </div>
                            </TableHead>
                            <TableHead className="hidden lg:table-cell">
                                <div className="flex items-center gap-2">
                                    <FolderOpen size={14} />
                                    Path
                                </div>
                            </TableHead>
                            <TableHead
                                className="cursor-pointer hidden lg:table-cell hover:bg-muted/80 transition-colors"
                                onClick={() => handleSort("created_at")}
                            >
                                <div className="flex items-center gap-2">
                                    <Calendar size={14} />
                                    Uploaded
                                    {sortField === "created_at" &&
                                        (sortDirection === "asc" ? <ChevronUp size={14} /> : <ChevronDown size={14} />)}
                                </div>
                            </TableHead>
                            <TableHead
                                className="cursor-pointer hidden sm:table-cell hover:bg-muted/80 transition-colors"
                                onClick={() => handleSort("size")}
                            >
                                <div className="flex items-center gap-2">
                                    Size
                                    {sortField === "size" &&
                                        (sortDirection === "asc" ? <ChevronUp size={14} /> : <ChevronDown size={14} />)}
                                </div>
                            </TableHead>
                            <TableHead className="text-right">Action</TableHead>
                        </TableRow>
                    </TableHeader>
                    <TableBody>
                        {sortedFiles.length === 0 ? (
                            <TableRow>
                                <TableCell colSpan={6} className="h-24 text-center text-muted-foreground">
                                    No files found.
                                </TableCell>
                            </TableRow>
                        ) : (
                            sortedFiles.map((file) => {
                                const isDownloading = downloading === file.id;
                                const ext = file.extension || file.name.split(".").pop()?.toLowerCase();
                                const dateTime = formatDateTime(file.created_at);
                                const relativeTime = getRelativeTime(file.created_at);

                                return (
                                    <TableRow key={file.id} className="hover:bg-muted/30">
                                        <TableCell className="text-center">
                                            <div className="flex justify-center text-muted-foreground">
                                                {ext === "pdf" ? <FileText size={18} /> : <File size={18} />}
                                            </div>
                                        </TableCell>
                                        <TableCell>
                                            <div className="flex flex-col gap-1">
                                                <span
                                                    className="font-medium truncate max-w-[150px] sm:max-w-[200px] md:max-w-[300px]"
                                                    title={file.name}
                                                >
                                                    {file.name}
                                                </span>
                                                <div className="flex items-center gap-2 text-[10px] text-muted-foreground flex-wrap">
                                                    <Badge variant="outline" className="text-[10px] px-1.5 py-0.5">
                                                        ID: {file.id}
                                                    </Badge>
                                                    {file.extension && (
                                                        <Badge variant="secondary" className="text-[10px] px-1.5 py-0.5">
                                                            .{file.extension}
                                                        </Badge>
                                                    )}
                                                    {getStatusBadge(file.object?.is_compressed)}
                                                    <span className="lg:hidden">• {relativeTime}</span>
                                                    <span className="sm:hidden">
                                                        • <SizeDisplay size={file.size} />
                                                    </span>
                                                </div>
                                            </div>
                                        </TableCell>
                                        <TableCell className="hidden lg:table-cell">
                                            <div className="flex items-center gap-2">
                                                <span
                                                    className="text-xs text-muted-foreground truncate max-w-[200px]"
                                                    title={file.full_path}
                                                >
                                                    {file.full_path || "/"}
                                                </span>
                                            </div>
                                        </TableCell>
                                        <TableCell className="hidden lg:table-cell">
                                            <div className="flex flex-col gap-1">
                                                <span className="text-sm font-medium">{dateTime.date}</span>
                                                <span className="text-xs text-muted-foreground">{dateTime.time}</span>
                                            </div>
                                        </TableCell>
                                        <TableCell className="hidden sm:table-cell text-xs">
                                            <SizeDisplay size={file.size} />
                                        </TableCell>
                                        <TableCell className="text-right">
                                            <Button
                                                size="sm"
                                                variant={isDownloading ? "secondary" : "default"}
                                                disabled={isDownloading}
                                                onClick={() => onDownload(file)}
                                                className="h-8 w-8 p-0 md:w-auto md:px-3"
                                                title="Download file"
                                            >
                                                {isDownloading ? (
                                                    <Loader2 size={16} className="animate-spin" />
                                                ) : (
                                                    <>
                                                        <Download size={16} className="md:mr-2" />
                                                        <span className="hidden md:inline">Download</span>
                                                    </>
                                                )}
                                            </Button>
                                        </TableCell>
                                    </TableRow>
                                );
                            })
                        )}
                    </TableBody>
                </Table>
            </div>
            <div className="p-4 border-t bg-muted/20 text-xs text-center text-muted-foreground">
                Showing {sortedFiles.length} file{sortedFiles.length !== 1 ? "s" : ""}
            </div>
        </div>
    );
}
