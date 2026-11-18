import { ChevronLeft, ChevronRight } from "lucide-react";

interface PaginationControlsProps {
    currentPage: number;
    totalPages: number;
    onPrev: () => void;
    onNext: () => void;
}

export function PaginationControls({ currentPage, totalPages, onPrev, onNext }: PaginationControlsProps) {
    const safeTotalPages = Math.max(1, totalPages);
    const safeCurrentPage = Math.min(Math.max(1, currentPage), safeTotalPages);

    return (
        <nav className="drive-pagination" aria-label="Directory pages">
            <button type="button" onClick={onPrev} disabled={safeCurrentPage === 1} className="btn-desktop px-3" aria-label="Previous page">
                <ChevronLeft size={16} />
            </button>
            <div className="drive-pagination-info" aria-live="polite">
                Page <span className="text-foreground">{safeCurrentPage}</span> of <span className="text-foreground">{safeTotalPages}</span>
            </div>
            <button
                type="button"
                onClick={onNext}
                disabled={safeCurrentPage === safeTotalPages}
                className="btn-desktop px-3"
                aria-label="Next page"
            >
                <ChevronRight size={16} />
            </button>
        </nav>
    );
}
