import { useEffect, useState, useMemo, Component, type ReactNode } from "react";
import { X, Download, FileText, Loader2, Search, Image as ImageIcon, ChevronDown } from "lucide-react";
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

export function FilePreviewModal({ isOpen, file, currentScope, onClose, onDownload }: FilePreviewModalProps) {
    const [loading, setLoading] = useState(false);
    const [content, setContent] = useState<string | null>(null);
    const [error, setError] = useState<string | null>(null);
    const [imageUrl, setImageUrl] = useState<string | null>(null);
    const [mediaUrl, setMediaUrl] = useState<string | null>(null); // pdf / video / audio
    const [mediaType, setMediaType] = useState<"pdf" | "video" | "audio" | null>(null);
    const [parsedJson, setParsedJson] = useState<any | null>(null);
    const [jsonQuery, setJsonQuery] = useState<string>("");
    const [wasDecompressed, setWasDecompressed] = useState(false);
    const [jpLoader, setJpLoader] = useState<any>(null);

    // Lazily load jsonpath only when the user starts typing a query
    useEffect(() => {
        if (jsonQuery && !jpLoader) {
            import("jsonpath").then((m) => setJpLoader(() => m.default ?? m)).catch(() => console.error("Failed to load jsonpath"));
        }
    }, [jsonQuery]);

    // Load file content whenever the modal opens or file changes
    useEffect(() => {
        if (!isOpen || !file) {
            setContent(null);
            setError(null);
            if (imageUrl) URL.revokeObjectURL(imageUrl);
            setImageUrl(null);
            if (mediaUrl) URL.revokeObjectURL(mediaUrl);
            setMediaUrl(null);
            setMediaType(null);
            setParsedJson(null);
            setJsonQuery("");
            setWasDecompressed(false);
            return;
        }

        const loadContent = async () => {
            setLoading(true);
            setError(null);
            setContent(null);

            const result = await fetchDriveFileContent(file.path, currentScope);
            if (isError(result)) {
                setError(result.error);
                setLoading(false);
                return;
            }

            const buffer = result.data;

            // ── Image path ───────────────────────────────────────────────
            if (isImageFile(file.name)) {
                const blob = new Blob([buffer], { type: getMimeType(file.name) });
                setImageUrl(URL.createObjectURL(blob));
                setLoading(false);
                return;
            }

            // ── PDF path ─────────────────────────────────────────────────
            if (isPdfFile(file.name)) {
                const blob = new Blob([buffer], { type: "application/pdf" });
                setMediaUrl(URL.createObjectURL(blob));
                setMediaType("pdf");
                setLoading(false);
                return;
            }

            // ── Video path ───────────────────────────────────────────────
            if (isVideoFile(file.name)) {
                const blob = new Blob([buffer], { type: getMimeType(file.name) });
                setMediaUrl(URL.createObjectURL(blob));
                setMediaType("video");
                setLoading(false);
                return;
            }

            // ── Audio path ───────────────────────────────────────────────
            if (isAudioFile(file.name)) {
                const blob = new Blob([buffer], { type: getMimeType(file.name) });
                setMediaUrl(URL.createObjectURL(blob));
                setMediaType("audio");
                setLoading(false);
                return;
            }

            // ── Text / binary path ───────────────────────────────────────
            const view = new Uint8Array(buffer as ArrayBuffer);
            let dataToDecode: Uint8Array = view;

            // Zstd magic: 0x28 0xB5 0x2F 0xFD
            const isZstd = view.length >= 4 && view[0] === 0x28 && view[1] === 0xb5 && view[2] === 0x2f && view[3] === 0xfd;

            setWasDecompressed(isZstd);

            if (isZstd) {
                try {
                    dataToDecode = decompress(view) as unknown as Uint8Array;
                } catch {
                    setError("Failed to decompress file.");
                    setLoading(false);
                    return;
                }
            }

            let text = new TextDecoder("utf-8").decode(dataToDecode);

            // Pretty-print JSON
            if (file.name.endsWith(".json") || file.name.endsWith(".json.zst")) {
                try {
                    const parsed = JSON.parse(text);
                    setParsedJson(parsed);
                    text = JSON.stringify(parsed, null, 2);
                } catch {
                    /* leave as-is */
                }
            }

            setContent(text);
            setLoading(false);
        };

        if (isTextFile(file.name) || isImageFile(file.name)) {
            loadContent();
        } else if (isPdfFile(file.name) || isVideoFile(file.name) || isAudioFile(file.name)) {
            loadContent();
        } else {
            setLoading(false);
            setError("Preview is not available for this file type.");
        }
    }, [isOpen, file, currentScope]);

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
        const blob = new Blob([content], { type: "text/plain" });
        const url = URL.createObjectURL(blob);
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

    return (
        <div className="preview-modal-overlay">
            <div className="preview-modal-window">
                {/* ── Header ───────────────────────────────────────── */}
                <div className="preview-modal-header">
                    <div className="preview-modal-title">
                        {isImageFile(file.name) || isVideoFile(file.name) ? (
                            <ImageIcon size={16} className="text-blue-400" />
                        ) : (
                            <FileText size={16} className="text-blue-400" />
                        )}
                        {file.name}
                    </div>

                    <div className="preview-modal-actions">
                        {/* JSONPath query bar – only for JSON */}
                        {parsedJson && (
                            <div className="flex items-center bg-[#1e1e1e] border border-white/10 rounded px-2 mr-2">
                                <Search size={14} className="text-white/40 mr-2" />
                                <input
                                    type="text"
                                    placeholder="JSONPath query (e.g. $.users[0])"
                                    className="bg-transparent text-xs text-white placeholder:text-white/30 border-none outline-none w-56 h-8 font-mono"
                                    value={jsonQuery}
                                    onChange={(e) => setJsonQuery(e.target.value)}
                                />
                            </div>
                        )}

                        {/* Download button – dropdown when file was decompressed */}
                        {wasDecompressed ? (
                            <DropdownMenu>
                                <DropdownMenuTrigger className="preview-modal-download">
                                    <Download size={14} />
                                    <span className="text-xs font-bold tracking-wider">DOWNLOAD</span>
                                    <ChevronDown size={14} className="ml-1 opacity-70" />
                                </DropdownMenuTrigger>
                                <DropdownMenuContent align="end" className="w-48 bg-[#252526] border-white/10 text-white shadow-xl">
                                    <DropdownMenuItem
                                        className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                                        onClick={handleDownloadDecompressed}
                                    >
                                        Download Decompressed
                                    </DropdownMenuItem>
                                    <DropdownMenuItem
                                        className="cursor-pointer hover:bg-white/10 focus:bg-white/10 py-2"
                                        onClick={() => onDownload(file)}
                                    >
                                        Download Original (.zst)
                                    </DropdownMenuItem>
                                </DropdownMenuContent>
                            </DropdownMenu>
                        ) : (
                            <button className="preview-modal-download" onClick={() => onDownload(file)}>
                                <Download size={14} />
                                <span className="text-xs font-bold tracking-wider">DOWNLOAD</span>
                            </button>
                        )}

                        <div className="w-px h-5 bg-white/10 mx-2" />
                        <button className="preview-modal-close" onClick={onClose}>
                            <X size={18} />
                        </button>
                    </div>
                </div>

                {/* ── Content ──────────────────────────────────────── */}
                <div className="preview-modal-content">
                    {loading ? (
                        <div className="preview-modal-loading">
                            <Loader2 size={36} className="animate-spin text-blue-500" />
                            <div className="text-xs font-mono uppercase tracking-[0.2em]">Loading...</div>
                        </div>
                    ) : error ? (
                        <div className="preview-modal-error">
                            <div className="preview-modal-error-msg">{error}</div>
                        </div>
                    ) : imageUrl ? (
                        <div className="w-full h-full flex items-center justify-center p-8 bg-[#1a1a1a]">
                            <img src={imageUrl} alt={file.name} className="max-w-full max-h-full object-contain drop-shadow-2xl" />
                        </div>
                    ) : mediaUrl && mediaType === "pdf" ? (
                        <iframe src={mediaUrl} title={file.name} className="w-full h-full border-0" style={{ background: "#fff" }} />
                    ) : mediaUrl && mediaType === "video" ? (
                        <div className="w-full h-full flex items-center justify-center bg-black">
                            <video
                                src={mediaUrl}
                                controls
                                autoPlay={false}
                                className="max-w-full max-h-full outline-none"
                                style={{ maxHeight: "calc(100vh - 120px)" }}
                            />
                        </div>
                    ) : mediaUrl && mediaType === "audio" ? (
                        <div className="w-full h-full flex flex-col items-center justify-center gap-6 bg-[#1a1a1a]">
                            <div className="flex flex-col items-center gap-3 text-white/60">
                                <div className="w-20 h-20 rounded-full bg-white/5 border border-white/10 flex items-center justify-center">
                                    <svg width="36" height="36" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                                        <path d="M9 18V5l12-2v13" />
                                        <circle cx="6" cy="18" r="3" />
                                        <circle cx="18" cy="16" r="3" />
                                    </svg>
                                </div>
                                <span className="text-sm font-mono opacity-60">{file.name}</span>
                            </div>
                            <audio src={mediaUrl} controls className="w-80" />
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
                                        padding: "1.5rem",
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
        </div>
    );
}
