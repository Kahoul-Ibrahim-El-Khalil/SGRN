// @/pages/download/components/SearchPanel.tsx
import { Search, FileText, FolderTree, FileType, Users, User, Clock } from "lucide-react";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Badge } from "@/components/ui/badge";
import type { FilterType } from "@/pages/download/types";

interface SearchPanelProps {
    filterType: FilterType;
    filterValue: string;
    limit: number;
    offset: number;
    loading: boolean;
    isAdmin: boolean;
    onFilterTypeChange: (type: FilterType) => void;
    onFilterValueChange: (value: string) => void;
    onLimitChange: (value: number) => void;
    onSearch: () => void;
    onNextPage: () => void;
    onPrevPage: () => void;
    onResetPage: () => void;
}

export function SearchPanel({
    filterType,
    filterValue,
    limit,
    offset,
    loading,
    isAdmin,
    onFilterTypeChange,
    onFilterValueChange,
    onLimitChange,
    onSearch,
    onNextPage,
    onPrevPage,
    onResetPage,
}: SearchPanelProps) {
    const filterOptions = [
        { id: "name" as const, label: "Filename", icon: FileText, adminOnly: false },
        { id: "extension" as const, label: "Extension", icon: FileType, adminOnly: false },
        { id: "path" as const, label: "Path Prefix", icon: FolderTree, adminOnly: false },
        { id: "session" as const, label: "Session ID", icon: Clock, adminOnly: false },
        { id: "user" as const, label: "User ID", icon: User, adminOnly: true },
        { id: "domain" as const, label: "Domain", icon: Users, adminOnly: true },
    ];

    const visibleFilters = filterOptions.filter((opt) => !opt.adminOnly || isAdmin);

    const getPlaceholder = () => {
        switch (filterType) {
            case "name":
                return "Enter filename (e.g., invoice, report)...";
            case "extension":
                return "Enter extension (e.g., pdf, docx)...";
            case "path":
                return "Enter path prefix (e.g., documents/2024)...";
            case "user":
                return "Enter user ID...";
            case "domain":
                return "Enter domain...";
            case "session":
                return "Enter session ID...";
            default:
                return "Enter search value...";
        }
    };

    const handleKeyPress = (e: React.KeyboardEvent) => {
        if (e.key === "Enter" && filterValue) {
            onSearch();
        }
    };

    const currentPage = Math.floor(offset / limit) + 1;

    return (
        <Card className="w-full shadow-sm">
            <CardHeader className="pb-3">
                <CardTitle className="text-sm font-bold uppercase tracking-widest text-muted-foreground">Search & Filter</CardTitle>
            </CardHeader>
            <CardContent className="space-y-6">
                {/* Filter Type Selection */}
                <div className="space-y-3">
                    <Label>Filter By</Label>
                    <div className="grid grid-cols-2 lg:grid-cols-3 gap-2">
                        {visibleFilters.map((opt) => (
                            <Button
                                key={opt.id}
                                variant={filterType === opt.id ? "default" : "outline"}
                                className="justify-start gap-2 h-auto py-3"
                                onClick={() => onFilterTypeChange(opt.id)}
                            >
                                <opt.icon size={18} />
                                <div className="flex flex-col items-start">
                                    <span className="font-bold text-sm">{opt.label}</span>
                                    {opt.adminOnly && (
                                        <Badge variant="secondary" className="text-[9px] px-1 py-0 mt-0.5">
                                            Admin
                                        </Badge>
                                    )}
                                </div>
                            </Button>
                        ))}
                    </div>
                </div>

                {/* Search Input */}
                <div className="p-4 bg-muted/30 rounded-lg border-2 border-dashed border-muted space-y-4">
                    <div className="flex flex-col md:flex-row gap-2 items-end">
                        <div className="w-full space-y-2">
                            <Label>Search Value</Label>
                            <Input
                                placeholder={getPlaceholder()}
                                value={filterValue}
                                onChange={(e) => onFilterValueChange(e.target.value)}
                                onKeyPress={handleKeyPress}
                                className="font-mono"
                            />
                            <p className="text-xs text-muted-foreground">
                                {filterType === "name" && "Searches for filenames containing this text (case-insensitive)"}
                                {filterType === "extension" && "Exact match for file extension (e.g., pdf, txt, docx)"}
                                {filterType === "path" && "Finds files whose path starts with this prefix"}
                                {filterType === "user" && "Filter files uploaded by specific user ID"}
                                {filterType === "domain" && "Filter files from specific domain"}
                                {filterType === "session" && "Filter files from a specific session ID. Leave empty for current session."}
                            </p>
                        </div>
                        <Button onClick={onSearch} disabled={loading} className="w-full md:w-auto min-w-[120px]">
                            {loading ? <Search className="animate-pulse" size={18} /> : <Search size={18} />}
                            <span className="ml-2">Search</span>
                        </Button>
                    </div>

                    {/* Quick Clear */}
                    {filterValue && (
                        <Button
                            variant="ghost"
                            size="sm"
                            onClick={() => {
                                onFilterValueChange("");
                                onSearch();
                            }}
                            className="w-full"
                        >
                            Clear filter
                        </Button>
                    )}
                </div>

                {/* Pagination Controls */}
                <div className="space-y-3 pt-2 border-t">
                    <div className="flex items-center justify-between">
                        <Label className="text-xs">Page {currentPage}</Label>
                        <Badge variant="outline" className="text-xs">
                            Showing from row {offset + 1}
                        </Badge>
                    </div>
                    <div className="flex gap-2">
                        <Button size="sm" variant="outline" onClick={onPrevPage} disabled={offset === 0} className="flex-1">
                            Previous
                        </Button>
                        <Button size="sm" variant="outline" onClick={onNextPage} className="flex-1">
                            Next
                        </Button>
                        <Button size="sm" variant="ghost" onClick={onResetPage} disabled={offset === 0}>
                            Reset
                        </Button>
                    </div>
                </div>

                {/* Results Per Page */}
                <div className="space-y-2">
                    <Label className="text-xs">Results Per Page</Label>
                    <select
                        value={limit}
                        onChange={(e) => onLimitChange(Number(e.target.value))}
                        className="w-full p-2 border rounded-md bg-background text-sm"
                    >
                        <option value={10}>10 rows</option>
                        <option value={25}>25 rows</option>
                        <option value={50}>50 rows</option>
                        <option value={100}>100 rows</option>
                    </select>
                </div>
            </CardContent>
        </Card>
    );
}
