import { useState, useRef, useEffect } from "react";
import { Upload as UploadIcon, XCircle, CheckCircle, Loader2, AlertCircle } from "lucide-react";
import { PageLayout } from "@/components/PageLayout";
import { getStorageConstraints, uploadFile } from "@/backend/api/storage";
import type { UploadProgress, UploadResult, UploadConstraints } from "@/pages/upload/types";

export default function UploadPage() {
    const [files, setFiles] = useState<File[]>([]);
    const [isUploading, setIsUploading] = useState(false);
    const [constraints, setConstraints] = useState<UploadConstraints | null>(null);
    const [progress, setProgress] = useState<UploadProgress>({});
    const [results, setResults] = useState<UploadResult[]>([]);

    const fileInputRef = useRef<HTMLInputElement | null>(null);
    const abortControllers = useRef<Map<string, AbortController>>(new Map());

    useEffect(() => {
        async function loadConfig() {
            const result = await getStorageConstraints();
            if (result.data) {
                const config = result.data;
                setConstraints(config);
            } else {
                console.error("Failed to load storage constraints", result.error);
            }
        }
        loadConfig();

        return () => {
            abortControllers.current.forEach((ctrl) => ctrl.abort());
        };
    }, []);

    const handleFileChange = (t_event: React.ChangeEvent<HTMLInputElement>) => {
        const selected = t_event.target.files ? Array.from(t_event.target.files) : [];
        setFiles(selected);
        setProgress({});
        setResults([]);
    };

    const handleUpload = async () => {
        if (!constraints) return;

        setIsUploading(true);
        setResults([]);

        const uploadPromises = files.map(async (file) => {
            const result = await uploadFile(file, constraints, (p) => {
                setProgress((prev) => ({ ...prev, [file.name]: p }));
            });

            if (result.data) {
                const res = result.data;
                setResults((prev) => [...prev, res]);
                return res;
            } else {
                const res: UploadResult = {
                    file: file.name,
                    success: false,
                    error: result.error || "Upload failed",
                };
                setResults((prev) => [...prev, res]);
                return res;
            }
        });

        await Promise.all(uploadPromises);
        setIsUploading(false);
        setFiles([]);
        if (fileInputRef.current) fileInputRef.current.value = "";
    };

    const handleCancel = () => {
        // Current uploadFile implementation doesn't support easy per-file cancellation yet,
        // but we can abort the whole page logic for simplicity or just reset state
        // To properly support cancel, we'd need to pass AbortSignal to uploadFile
        setFiles([]);
        setProgress({});
        setResults([]);
        setIsUploading(false);
        if (fileInputRef.current) fileInputRef.current.value = "";
    };

    return (
        <PageLayout>
            <div className="upload-page-container">
                <div className="upload-page-card">
                    {/* Header */}
                    <div className="upload-page-header">
                        <h2 className="upload-page-title">FILE UPLOAD SYSTEM</h2>
                    </div>

                    {/* File Input */}
                    <input ref={fileInputRef} type="file" multiple onChange={handleFileChange} className="hidden" id="fileInput" />

                    <label htmlFor="fileInput" className="upload-page-dropzone">
                        <UploadIcon size={48} className="upload-page-dropzone-icon" />
                        <span className="upload-page-dropzone-title">CLICK TO SELECT FILES</span>
                        <span className="upload-page-dropzone-subtitle">OR DRAG AND DROP HERE</span>
                    </label>

                    {/* Files List */}
                    {files.length > 0 && (
                        <div className="upload-page-files-list">
                            {files.map((file, idx) => {
                                const res = results.find((r) => r.file === file.name);
                                const fileProgress = progress[file.name] || 0;

                                return (
                                    <div key={file.name + idx} className="upload-page-file-item">
                                        <div className="upload-page-file-content">
                                            <div className="upload-page-file-info">
                                                <div className="upload-page-file-name">{file.name}</div>
                                                <div className="upload-page-file-meta">
                                                    <span className="upload-page-file-size">{(file.size / 1024 / 1024).toFixed(2)} MB</span>
                                                </div>

                                                {isUploading && (
                                                    <>
                                                        <div className="upload-page-progress-bg">
                                                            <div
                                                                className="upload-page-progress-fill"
                                                                style={{ width: `${fileProgress}%` }}
                                                            />
                                                        </div>
                                                        {fileProgress > 0 && fileProgress < 100 && (
                                                            <div className="upload-page-progress-text">{fileProgress}% UPLOADED</div>
                                                        )}
                                                    </>
                                                )}

                                                {res?.error && <div className="upload-page-file-error">ERROR: {res.error}</div>}
                                            </div>

                                            <div className="upload-page-file-icon">
                                                {res?.is_duplicate ? (
                                                    <AlertCircle className="text-yellow-600 dark:text-yellow-400" size={24} />
                                                ) : res?.success ? (
                                                    <CheckCircle className="text-green-600 dark:text-green-400" size={24} />
                                                ) : res?.success === false ? (
                                                    <XCircle className="text-red-600 dark:text-red-400" size={24} />
                                                ) : isUploading ? (
                                                    <Loader2 className="animate-spin text-primary" size={24} />
                                                ) : (
                                                    <button
                                                        onClick={() => setFiles((prev) => prev.filter((f) => f.name !== file.name))}
                                                        className="upload-page-remove-btn"
                                                    >
                                                        <XCircle size={20} />
                                                    </button>
                                                )}
                                            </div>
                                        </div>
                                    </div>
                                );
                            })}
                        </div>
                    )}

                    {/* Success Alert */}
                    {results.length > 0 && (
                        <div className="upload-page-alert-success">
                            <CheckCircle size={20} />
                            <span className="upload-page-alert-text">
                                UPLOAD COMPLETE! {results.filter((r) => r.success).length} OF {results.length} FILES UPLOADED
                            </span>
                        </div>
                    )}

                    {/* Action Buttons */}
                    <div className="upload-page-actions">
                        <button onClick={handleCancel} disabled={isUploading} className="upload-page-btn-secondary">
                            CANCEL
                        </button>
                        <button
                            onClick={handleUpload}
                            disabled={!files.length || isUploading || !constraints}
                            className="upload-page-btn-primary"
                        >
                            {isUploading ? (
                                <>
                                    <Loader2 className="animate-spin" size={16} />
                                    <span>UPLOADING...</span>
                                </>
                            ) : (
                                <>
                                    <UploadIcon size={16} />
                                    <span>START UPLOAD</span>
                                </>
                            )}
                        </button>
                    </div>
                </div>
            </div>
        </PageLayout>
    );
}
