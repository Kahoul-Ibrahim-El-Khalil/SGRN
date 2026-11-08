import { useEffect, useState, useMemo, useRef, useCallback, Component, type ReactNode } from "react";
import { createPortal } from "react-dom";
import { X, Download, FileText, Loader2, Search, Image as ImageIcon, ChevronDown, Music } from "lucide-react";
import { decompress } from "fzstd";
import { Light as SyntaxHighlighter } from "react-syntax-highlighter";
import { vs2015 } from "react-syntax-highlighter/dist/esm/styles/hljs";
// Register only the languages we actually need – avoids crashes from missing Prism grammars
import langJson from "react-syntax-highlighter/dist/esm/languages/hljs/json";
import langJs from "react-syntax-highlighter/dist/esm/languages/hljs/javascript";
import langTs from "react-syntax-highlighter/dist/esm/languages/hljs/typescript";
import langPy from "react-syntax-highlighter/dist/esm/languages/hljs/python";
import langCpp from "react-syntax-highlighter/dist/esm/languages/hljs/cpp";
import langCss from "react-syntax-highlighter/dist/esm/languages/hljs/css";
import langHtml from "react-syntax-highlighter/dist/esm/languages/hljs/xml"; // hljs uses xml for html
import langYaml from "react-syntax-highlighter/dist/esm/languages/hljs/yaml";
import langBash from "react-syntax-highlighter/dist/esm/languages/hljs/bash";
import langMarkdown from "react-syntax-highlighter/dist/esm/languages/hljs/markdown";
import langPlaintext from "react-syntax-highlighter/dist/esm/languages/hljs/plaintext";
import type { DriveFile } from "@sgrn/types";
import { fetchDriveFileContent, type StorageScope } from "@/pages/drive/api";
import { isError } from "@sgrn/types";
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from "@/components/ui/dropdown-menu";

SyntaxHighlighter.registerLanguage("json", langJson);
SyntaxHighlighter.registerLanguage("javascript", langJs);
SyntaxHighlighter.registerLanguage("typescript", langTs);
SyntaxHighlighter.registerLanguage("python", langPy);
SyntaxHighlighter.registerLanguage("cpp", langCpp);
SyntaxHighlighter.registerLanguage("css", langCss);
SyntaxHighlighter.registerLanguage("html", langHtml);
SyntaxHighlighter.registerLanguage("xml", langHtml);
SyntaxHighlighter.registerLanguage("yaml", langYaml);
SyntaxHighlighter.registerLanguage("bash", langBash);
SyntaxHighlighter.registerLanguage("markdown", langMarkdown);
SyntaxHighlighter.registerLanguage("plaintext", langPlaintext);

// ─── Error Boundary ──────────────────────────────────────────────────────────
// Catches any rendering crash inside the highlighter and shows a plain <pre>
// instead of white-screening the whole app.
interface EBState {
    crashed: boolean;
}
class HighlightBoundary extends Component<{ children: ReactNode; fallback: ReactNode }, EBState> {
    state: EBState = { crashed: false };
    static getDerivedStateFromError() {
        return { crashed: true };
    }
    render() {
        return this.state.crashed ? this.props.fallback : this.props.children;
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

interface FilePreviewModalProps {
    isOpen: boolean;
    file: DriveFile | null;
    currentScope: StorageScope;
    onClose: () => void;
    onDownload: (file: DriveFile) => void;
}

const TEXT_EXTENSIONS = [
    "txt",
    "md",
    "csv",
    "json",
    "js",
    "ts",
    "tsx",
    "jsx",
    "py",
    "cpp",
    "c",
    "h",
    "hpp",
    "css",
    "html",
    "xml",
    "yaml",
    "yml",
    "log",
    "sh",
    "bat",
];
const IMAGE_EXTENSIONS = ["jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "ico"];
const VIDEO_EXTENSIONS = ["mp4", "webm", "ogg", "mov", "avi", "mkv"];
const AUDIO_EXTENSIONS = ["mp3", "wav", "ogg", "flac", "aac", "m4a", "opus"];

function getExtension(filename: string): string {
    return filename.split(".").pop()?.toLowerCase() || "";
}

function isTextFile(filename: string): boolean {
    const ext = getExtension(filename);
    if (TEXT_EXTENSIONS.includes(ext)) return true;
    if (filename.endsWith(".json.zst") || filename.endsWith(".txt.zst") || ext === "zst") return true;
    return false;
}

function isImageFile(filename: string): boolean {
    return IMAGE_EXTENSIONS.includes(getExtension(filename));
}

function isPdfFile(filename: string): boolean {
    return getExtension(filename) === "pdf";
}

function isVideoFile(filename: string): boolean {
    return VIDEO_EXTENSIONS.includes(getExtension(filename));
}

function isAudioFile(filename: string): boolean {
    return AUDIO_EXTENSIONS.includes(getExtension(filename));
}

function getMimeType(filename: string): string {
    const ext = getExtension(filename);
    const map: Record<string, string> = {
        // images
        svg: "image/svg+xml",
        jpg: "image/jpeg",
        jpeg: "image/jpeg",
        png: "image/png",
        gif: "image/gif",
        webp: "image/webp",
        bmp: "image/bmp",
        ico: "image/x-icon",
        // pdf
        pdf: "application/pdf",
        // video
        mp4: "video/mp4",
        webm: "video/webm",
        ogv: "video/ogg",
        mov: "video/quicktime",
        avi: "video/x-msvideo",
        mkv: "video/x-matroska",
        // audio
        mp3: "audio/mpeg",
        wav: "audio/wav",
        ogg: "audio/ogg",
        flac: "audio/flac",
        aac: "audio/aac",
        m4a: "audio/mp4",
        opus: "audio/opus",
    };
    return map[ext] ?? "application/octet-stream";
}

function getLanguage(filename: string): string {
    const ext = getExtension(filename);
    if (filename.endsWith(".json") || filename.endsWith(".json.zst")) return "json";
    if (ext === "js" || ext === "jsx") return "javascript";
    if (ext === "ts" || ext === "tsx") return "typescript";
    if (ext === "py") return "python";
    if (ext === "cpp" || ext === "c" || ext === "h" || ext === "hpp") return "cpp";
    if (ext === "css") return "css";
    if (ext === "html") return "html";
    if (ext === "xml") return "xml";
    if (ext === "yaml" || ext === "yml") return "yaml";
    if (ext === "sh" || ext === "bat") return "bash";
    if (ext === "md") return "markdown";
    return "plaintext";
}

// ─── Component ───────────────────────────────────────────────────────────────

type PreviewMedia = { url: string; kind: "image" | "pdf" | "video" | "audio" };

export function FilePreviewModal({ isOpen, file, currentScope, onClose, onDownload }: FilePreviewModalProps) {
    const [loading, setLoading] = useState(false);
    const [content, setContent] = useState<string | null>(null);
    const [error, setError] = useState<string | null>(null);
    const [media, setMedia] = useState<PreviewMedia | null>(null);
    const [parsedJson, setParsedJson] = useState<any | null>(null);
    const [jsonQuery, setJsonQuery] = useState<string>("");
    const [wasDecompressed, setWasDecompressed] = useState(false);
    const [jpLoader, setJpLoader] = useState<any>(null);
    const objectUrlRef = useRef<string | null>(null);

    const releaseObjectUrl = useCallback(() => {
        if (objectUrlRef.current) {
            URL.revokeObjectURL(objectUrlRef.current);
            objectUrlRef.current = null;
        }
    }, []);

    // Lazily load jsonpath only when the user starts typing a query
    useEffect(() => {
        if (jsonQuery && !jpLoader) {
            import("jsonpath").then((m) => setJpLoader(() => m.default ?? m)).catch(() => console.error("Failed to load jsonpath"));
        }
    }, [jsonQuery, jpLoader]);

    // Escape to close + lock page scroll while open
    useEffect(() => {
        if (!isOpen) return;
        const onKey = (e: KeyboardEvent) => {
            if (e.key === "Escape") onClose();
        };
        const prevOverflow = document.body.style.overflow;
        document.body.style.overflow = "hidden";
        window.addEventListener("keydown", onKey);
        return () => {
            window.removeEventListener("keydown", onKey);
            document.body.style.overflow = prevOverflow;
        };
    }, [isOpen, onClose]);

    // Load content whenever the modal opens or the file changes
    useEffect(() => {
        setContent(null);
        setError(null);
        setMedia(null);
        setParsedJson(null);
        setJsonQuery("");
        setWasDecompressed(false);

        if (!isOpen || !file) {
            setLoading(false);
            return;
        }

        const kind: PreviewMedia["kind"] | null = isImageFile(file.name)
            ? "image"
            : isPdfFile(file.name)
              ? "pdf"
              : isVideoFile(file.name)
                ? "video"
                : isAudioFile(file.name)
                  ? "audio"
                  : null;

        if (!kind && !isTextFile(file.name)) {
            setLoading(false);
            setError("Preview is not available for this file type.");
            return;
        }

        let cancelled = false;
        setLoading(true);

        (async () => {
            const result = await fetchDriveFileContent(file.path, currentScope);
            if (cancelled) return;
            if (isError(result)) {
                setError(result.error);
                setLoading(false);
                return;
            }
            const buffer = result.data;

            const view = new Uint8Array(buffer);
            // Zstd magic: 0x28 0xB5 0x2F 0xFD — sniff for every preview kind,
            // not just text: an object stored compressed must never reach the
            // media blob, or the PDF viewer (native iframe) renders a blank
            // page instead of the document.
            const isZstd = view.length >= 4 && view[0] === 0x28 && view[1] === 0xb5 && view[2] === 0x2f && view[3] === 0xfd;
            let bytes: Uint8Array<ArrayBuffer> = view;
            if (isZstd) {
                try {
                    bytes = decompress(view) as unknown as Uint8Array<ArrayBuffer>;
                } catch {
                    setError("Failed to decompress file.");
                    setLoading(false);
                    return;
                }
            }

            if (kind) {
                const url = URL.createObjectURL(new Blob([bytes], { type: getMimeType(file.name) }));
                objectUrlRef.current = url;
                setMedia({ url, kind });
                setLoading(false);
                return;
            }

            let text = new TextDecoder("utf-8").decode(bytes);
            let json: any = null;
            if (file.name.endsWith(".json") || file.name.endsWith(".json.zst")) {
                try {
                    json = JSON.parse(text);
                    text = JSON.stringify(json, null, 2);
                } catch {
                    /* leave as-is */
                }
            }

            setWasDecompressed(isZstd);
            setParsedJson(json);
            setContent(text);
            setLoading(false);
        })();

        return () => {
            cancelled = true;
            releaseObjectUrl();
        };
    }, [isOpen, file, currentScope, releaseObjectUrl]);

    const displayContent = useMemo(() => {
        if (!parsedJson || !jsonQuery) return content;
        if (!jpLoader) return `// Loading JSONPath engine...\n\n${content}`;
        try {
            return JSON.stringify(jpLoader.query(parsedJson, jsonQuery), null, 2);
        } catch (e) {
            return `// Invalid JSONPath query: ${e}\n\n${content}`;
        }
    }, [content, parsedJson, jsonQuery, jpLoader]);

    const handleDownloadDecompressed = () => {
        if (!content || !file) return;
        const url = URL.createObjectURL(new Blob([content], { type: "text/plain" }));
        const a = document.createElement("a");
        a.href = url;
        a.download = file.name.endsWith(".zst") ? file.name.slice(0, -4) : file.name;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    };

    if (!isOpen || !file) return null;

    const lang = getLanguage(file.name);
    const HeaderIcon = isImageFile(file.name) || isVideoFile(file.name) ? ImageIcon : FileText;

    return createPortal(
        <div
            className="preview-modal-overlay"
            role="presentation"
            onMouseDown={(e) => {
                if (e.target === e.currentTarget) onClose();
            }}
        >
            <div className="preview-modal-window" role="dialog" aria-modal="true" aria-label={`Preview of ${file.name}`}>
                <div className="preview-modal-header">
                    <div className="preview-modal-title">
                        <HeaderIcon size={16} />
                        <span>{file.name}</span>
                    </div>

                    <div className="preview-modal-actions">
                        {parsedJson && (
                            <label className="preview-modal-search">
                                <Search size={14} />
                                <input
                                    type="text"
                                    placeholder="JSONPath query (e.g. $.users[0])"
                                    value={jsonQuery}
                                    onChange={(e) => setJsonQuery(e.target.value)}
                                />
                            </label>
                        )}

                        {wasDecompressed ? (
                            <DropdownMenu>
                                <DropdownMenuTrigger className="preview-modal-download">
                                    <Download size={14} />
                                    <span>DOWNLOAD</span>
                                    <ChevronDown size={14} />
                                </DropdownMenuTrigger>
                                <DropdownMenuContent align="end" className="preview-modal-menu">
                                    <DropdownMenuItem className="preview-modal-menu-item" onClick={handleDownloadDecompressed}>
                                        Download Decompressed
                                    </DropdownMenuItem>
                                    <DropdownMenuItem className="preview-modal-menu-item" onClick={() => onDownload(file)}>
                                        Download Original (.zst)
                                    </DropdownMenuItem>
                                </DropdownMenuContent>
                            </DropdownMenu>
                        ) : (
                            <button type="button" className="preview-modal-download" onClick={() => onDownload(file)}>
                                <Download size={14} />
                                <span>DOWNLOAD</span>
                            </button>
                        )}

                        <button
                            type="button"
                            className="preview-modal-close"
                            onClick={onClose}
                            aria-label="Close preview"
                            title="Close (Esc)"
                        >
                            <X size={18} />
                        </button>
                    </div>
                </div>

                <div className="preview-modal-content">
                    {loading ? (
                        <div className="preview-modal-loading">
                            <Loader2 size={32} className="drive-spin" />
                            <span>Loading...</span>
                        </div>
                    ) : error ? (
                        <div className="preview-modal-error">
                            <div className="preview-modal-error-msg">{error}</div>
                        </div>
                    ) : media?.kind === "image" ? (
                        <div className="preview-modal-media">
                            <img src={media.url} alt={file.name} />
                        </div>
                    ) : media?.kind === "pdf" ? (
                        <iframe className="preview-modal-pdf" src={media.url} title={file.name} />
                    ) : media?.kind === "video" ? (
                        <div className="preview-modal-media">
                            <video src={media.url} controls />
                        </div>
                    ) : media?.kind === "audio" ? (
                        <div className="preview-modal-media preview-modal-audio">
                            <Music size={40} />
                            <span>{file.name}</span>
                            <audio src={media.url} controls />
                        </div>
                    ) : content !== null ? (
                        <div className="preview-modal-scroll">
                            <HighlightBoundary
                                fallback={
                                    <pre
                                        style={{
                                            margin: 0,
                                            padding: "1.5rem",
                                            fontSize: "13px",
                                            fontFamily: "monospace",
                                            color: "#d4d4d4",
                                            whiteSpace: "pre-wrap",
                                            wordBreak: "break-all",
                                        }}
                                    >
                                        {displayContent}
                                    </pre>
                                }
                            >
                                <SyntaxHighlighter
                                    language={lang}
                                    style={vs2015}
                                    showLineNumbers={true}
                                    wrapLines={true}
                                    wrapLongLines={false}
                                    customStyle={{
                                        margin: 0,
                                        padding: "1rem 1.25rem",
                                        fontSize: "13px",
                                        fontFamily:
                                            'ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace',
                                        background: "transparent",
                                        minHeight: "100%",
                                    }}
                                    lineNumberStyle={{
                                        minWidth: "3.5em",
                                        paddingRight: "1em",
                                        color: "#858585",
                                        textAlign: "right",
                                        borderRight: "1px solid #404040",
                                        marginRight: "1.5em",
                                    }}
                                >
                                    {displayContent!}
                                </SyntaxHighlighter>
                            </HighlightBoundary>
                        </div>
                    ) : null}
                </div>
            </div>
        </div>,
        document.body,
    );
}
