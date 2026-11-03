// @/pages/drive/components/Breadcrumb.tsx
import { ChevronRight } from "lucide-react";
import type { DrivePathNode } from "@sgrn/types";
import { toDisplayDrivePath, domainDisplaySegment, DOMAIN_PATH_PREFIX } from "@/pages/drive/pathDisplay";

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
    // Domain namespaces are rendered as real handles: canonical
    // /domains/<name>/<rest> displays as @<name>/<rest>. The synthetic
    // domain segment navigates back to the namespace root; segments deeper
    // in the trail belong to the in-domain directory tree (their storage
    // rows do not carry the /domains/<name> prefix, so no duplication).
    const domainSegment = domainDisplaySegment(path);

    // Fallback when no trail: labels come from the *display* form, segment
    // paths are reconstructed from the canonical path for navigation.
    const isDomainPath = path.startsWith(DOMAIN_PATH_PREFIX);
    const rawPieces = path.split("/").filter(Boolean);
    const displayPieces = (isDomainPath ? toDisplayDrivePath(path) : path).split("/").filter(Boolean);
    const pieceOffset = isDomainPath ? 1 : 0; // raw carries the extra "domains" node
    const segments =
        trail && trail.length > 0
            ? trail
            : path === "/"
              ? []
              : displayPieces.map((label, k) => ({
                    name: label,
                    path: "/" + rawPieces.slice(0, k + 1 + pieceOffset).join("/"),
                }));

    const rendered = domainSegment && trail && trail.length > 0 ? [domainSegment, ...segments] : segments;

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

            {rendered.map((segment, index) => {
                const segmentPath = segment.path;
                const isLast = index === rendered.length - 1;
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
