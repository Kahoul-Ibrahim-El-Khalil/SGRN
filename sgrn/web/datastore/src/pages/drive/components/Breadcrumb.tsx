// @/pages/drive/components/Breadcrumb.tsx
import { ChevronRight } from "lucide-react";
import type { DrivePathNode } from "@sgrn/types";

interface BreadcrumbProps {
    path: string;
    onNavigate: (path: string) => void;
    trail?: DrivePathNode[];
    onDropToPath?: (target: DrivePathNode | null, e: React.DragEvent) => void;
}

function getBreadcrumbLabel(segment: DrivePathNode): string {
    // If it's a service, display_name is the human name
    if (segment.display_name && segment.display_name !== segment.name) {
        return segment.display_name;
    }
    return segment.display_name || segment.name;
}

export function Breadcrumb({ path, onNavigate, trail, onDropToPath }: BreadcrumbProps) {
    const segments =
        trail && trail.length > 0
            ? trail
            : path === "/"
              ? []
              : path
                    .split("/")
                    .filter(Boolean)
                    .map((segment, index) => ({
                        name: segment,
                        path:
                            "/" +
                            path
                                .split("/")
                                .filter(Boolean)
                                .slice(0, index + 1)
                                .join("/"),
                    }));

    return (
        <nav className="drive-breadcrumb" aria-label="Breadcrumb">
            <button
                className="drive-breadcrumb-item drive-breadcrumb-root"
                onClick={() => onNavigate("/")}
                onDragOver={(e) => {
                    if (!onDropToPath) return;
                    e.preventDefault();
                    e.currentTarget.classList.add("bg-white/10");
                }}
                onDragLeave={(e) => {
                    e.currentTarget.classList.remove("bg-white/10");
                }}
                onDrop={(e) => {
                    if (!onDropToPath) return;
                    e.preventDefault();
                    e.stopPropagation();
                    e.currentTarget.classList.remove("bg-white/10");
                    onDropToPath(null, e);
                }}
                title="Go to root"
            ></button>

            {segments.map((segment, index) => {
                const segmentPath = segment.path;
                const isLast = index === segments.length - 1;
                const label = "display_name" in segment ? getBreadcrumbLabel(segment as DrivePathNode) : segment.name;

                return (
                    <span key={segmentPath} className="drive-breadcrumb-segment">
                        <ChevronRight size={14} className="drive-breadcrumb-separator" />
                        <button
                            className={`drive-breadcrumb-item ${isLast ? "drive-breadcrumb-current" : ""}`}
                            onClick={() => onNavigate(segmentPath)}
                            disabled={isLast}
                            onDragOver={(e) => {
                                if (!onDropToPath || isLast) return;
                                e.preventDefault();
                                e.currentTarget.classList.add("bg-white/10");
                            }}
                            onDragLeave={(e) => {
                                e.currentTarget.classList.remove("bg-white/10");
                            }}
                            onDrop={(e) => {
                                if (!onDropToPath || isLast) return;
                                e.preventDefault();
                                e.stopPropagation();
                                e.currentTarget.classList.remove("bg-white/10");
                                onDropToPath(segment as DrivePathNode, e);
                            }}
                        >
                            {label}
                        </button>
                    </span>
                );
            })}
        </nav>
    );
}
