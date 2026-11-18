import { Archive, File as FileIcon, FileCode, FileImage, FileSpreadsheet, FileText, Music, Video } from "lucide-react";
import type { LucideIcon } from "lucide-react";

type FileIconStyle = "grid" | "list";

interface FileIconConfig {
    icon: LucideIcon;
    gridClass: string;
    listClass: string;
}

const FILE_ICON_BY_EXTENSION: Readonly<Record<string, FileIconConfig>> = {
    jpg: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    jpeg: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    png: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    gif: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    webp: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    svg: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    bmp: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    ico: { icon: FileImage, gridClass: "file-grid-icon--image", listClass: "drive-icon-image" },
    js: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    ts: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    tsx: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    jsx: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    py: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    cpp: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    c: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    h: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    hpp: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    css: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    html: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    json: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    xml: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    yaml: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    yml: { icon: FileCode, gridClass: "file-grid-icon--code", listClass: "drive-icon-code" },
    csv: { icon: FileSpreadsheet, gridClass: "file-grid-icon--sheet", listClass: "drive-icon-spreadsheet" },
    xls: { icon: FileSpreadsheet, gridClass: "file-grid-icon--sheet", listClass: "drive-icon-spreadsheet" },
    xlsx: { icon: FileSpreadsheet, gridClass: "file-grid-icon--sheet", listClass: "drive-icon-spreadsheet" },
    ods: { icon: FileSpreadsheet, gridClass: "file-grid-icon--sheet", listClass: "drive-icon-spreadsheet" },
    mp3: { icon: Music, gridClass: "file-grid-icon--audio", listClass: "drive-icon-audio" },
    wav: { icon: Music, gridClass: "file-grid-icon--audio", listClass: "drive-icon-audio" },
    ogg: { icon: Music, gridClass: "file-grid-icon--audio", listClass: "drive-icon-audio" },
    flac: { icon: Music, gridClass: "file-grid-icon--audio", listClass: "drive-icon-audio" },
    aac: { icon: Music, gridClass: "file-grid-icon--audio", listClass: "drive-icon-audio" },
    m4a: { icon: Music, gridClass: "file-grid-icon--audio", listClass: "drive-icon-audio" },
    mp4: { icon: Video, gridClass: "file-grid-icon--video", listClass: "drive-icon-video" },
    avi: { icon: Video, gridClass: "file-grid-icon--video", listClass: "drive-icon-video" },
    mkv: { icon: Video, gridClass: "file-grid-icon--video", listClass: "drive-icon-video" },
    mov: { icon: Video, gridClass: "file-grid-icon--video", listClass: "drive-icon-video" },
    wmv: { icon: Video, gridClass: "file-grid-icon--video", listClass: "drive-icon-video" },
    webm: { icon: Video, gridClass: "file-grid-icon--video", listClass: "drive-icon-video" },
    zip: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    rar: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    "7z": { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    tar: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    gz: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    bz2: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    xz: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    zst: { icon: Archive, gridClass: "file-grid-icon--archive", listClass: "drive-icon-archive" },
    pdf: { icon: FileText, gridClass: "file-grid-icon--pdf", listClass: "drive-icon-pdf" },
};

const DEFAULT_FILE_ICON: FileIconConfig = {
    icon: FileIcon,
    gridClass: "file-grid-icon--generic",
    listClass: "drive-icon-generic",
};

export function DriveFileIcon({ extension, variant = "grid" }: { extension: string; variant?: FileIconStyle }) {
    const normalized = extension.toLowerCase().replace(/^\./, "");
    const config = FILE_ICON_BY_EXTENSION[normalized] ?? DEFAULT_FILE_ICON;
    const Icon = config.icon;
    const className = variant === "grid" ? config.gridClass : config.listClass;
    const size = variant === "grid" ? 20 : 18;

    return <Icon size={size} className={variant === "grid" ? `file-grid-icon ${className}` : `drive-file-icon ${className}`} />;
}
