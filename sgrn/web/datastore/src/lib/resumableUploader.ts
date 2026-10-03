import { StorageBackendApiEndpoints } from "../backend/endpoints";

export interface ChunkUploadProgress {
    uploadId: string;
    filename: string;
    totalSize: number;
    chunkSize: number;
    totalChunks: number;
    uploadedChunks: number;
    percent: number;
    status: "idle" | "uploading" | "paused" | "completed" | "error";
    error?: string;
}

export class ResumableUploader {
    private file: File;
    private targetPath: string;
    private chunkSize: number; // default 5MB
    private uploadId: string | null = null;
    private status: ChunkUploadProgress["status"] = "idle";
    private onProgressCallback?: (progress: ChunkUploadProgress) => void;
    private isAborted = false;

    constructor(file: File, targetPath: string = "/", chunkSize: number = 5 * 1024 * 1024) {
        this.file = file;
        this.targetPath = targetPath;
        this.chunkSize = chunkSize;
    }

    public onProgress(callback: (progress: ChunkUploadProgress) => void) {
        this.onProgressCallback = callback;
    }

    private emitProgress(uploadedChunks: number, totalChunks: number, error?: string) {
        const percent = Math.min(100, Math.round((uploadedChunks / Math.max(totalChunks, 1)) * 100));
        if (this.onProgressCallback) {
            this.onProgressCallback({
                uploadId: this.uploadId || "",
                filename: this.file.name,
                totalSize: this.file.size,
                chunkSize: this.chunkSize,
                totalChunks,
                uploadedChunks,
                percent,
                status: this.status,
                error,
            });
        }
    }

    /** Upload a single chunk with retry-backoff on 429 / 5xx. */
    private async uploadChunkWithRetry(
        authenticatedFetch: (url: string, init?: RequestInit) => Promise<Response>,
        chunkUrl: string,
        chunkBlob: Blob,
        chunkIdx: number,
        totalChunks: number,
    ): Promise<void> {
        const MAX_RETRIES = 5;
        const BASE_DELAY_MS = 1000;

        for (let attempt = 0; attempt <= MAX_RETRIES; attempt++) {
            if (this.isAborted) throw new Error("Upload aborted");

            const resp = await authenticatedFetch(chunkUrl, {
                method: "PUT",
                headers: { "Content-Type": "application/octet-stream" },
                body: chunkBlob,
            });

            if (resp.ok) return;

            // 409 = duplicate — propagate immediately, no retry.
            if (resp.status === 409) {
                const body = await resp.json().catch(() => ({}));
                throw new Error(body.message || "A file with that name already exists.");
            }

            // 429 = rate-limited — respect Retry-After header or use exponential backoff.
            if (resp.status === 429 && attempt < MAX_RETRIES) {
                const retryAfter = parseInt(resp.headers.get("Retry-After") ?? "0", 10);
                const delay = retryAfter > 0 ? retryAfter * 1000 : Math.min(BASE_DELAY_MS * Math.pow(2, attempt), 30_000);
                await new Promise((r) => setTimeout(r, delay));
                continue;
            }

            // 5xx — retry with exponential backoff.
            if (resp.status >= 500 && attempt < MAX_RETRIES) {
                await new Promise((r) => setTimeout(r, Math.min(BASE_DELAY_MS * Math.pow(2, attempt), 30_000)));
                continue;
            }

            const body = await resp.json().catch(() => ({}));
            throw new Error(body.message || `Chunk ${chunkIdx + 1}/${totalChunks} failed (HTTP ${resp.status})`);
        }
        throw new Error(`Chunk ${chunkIdx + 1}/${totalChunks} failed after retries`);
    }

    public async start(authenticatedFetch: (url: string, init?: RequestInit) => Promise<Response>): Promise<boolean> {
        this.status = "uploading";
        this.isAborted = false;

        const totalChunks = Math.ceil(this.file.size / this.chunkSize);

        try {
            // 1. Initialize upload session
            const initResp = await authenticatedFetch(StorageBackendApiEndpoints.UPLOAD_INIT, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({
                    filename: this.file.name,
                    target_path: this.targetPath,
                    mime_type: this.file.type || "application/octet-stream",
                    total_size: this.file.size,
                    chunk_size: this.chunkSize,
                }),
            });

            if (!initResp.ok) {
                const errData = await initResp.json().catch(() => ({}));
                throw new Error(errData.message || `Failed to initialize upload session (HTTP ${initResp.status})`);
            }

            const initData = await initResp.json();
            this.uploadId = initData.upload_id;

            this.emitProgress(0, totalChunks);

            // 2. Upload chunks sequentially with per-chunk retry on transient errors
            for (let chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
                if (this.isAborted) return false;
                if ((this.status as string) === "paused") {
                    this.emitProgress(chunkIdx, totalChunks);
                    return false;
                }

                const startByte = chunkIdx * this.chunkSize;
                const endByte = Math.min(this.file.size, startByte + this.chunkSize);
                const chunkBlob = this.file.slice(startByte, endByte);

                const chunkUrl = `${StorageBackendApiEndpoints.UPLOAD_CHUNK}?upload_id=${encodeURIComponent(this.uploadId!)}&chunk_index=${chunkIdx}`;

                await this.uploadChunkWithRetry(authenticatedFetch, chunkUrl, chunkBlob, chunkIdx, totalChunks);

                this.emitProgress(chunkIdx + 1, totalChunks);
            }

            // 3. Complete upload — server assembles chunks, hashes, and pushes to S3
            const completeResp = await authenticatedFetch(StorageBackendApiEndpoints.UPLOAD_COMPLETE, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ upload_id: this.uploadId }),
            });

            if (!completeResp.ok) {
                const errData = await completeResp.json().catch(() => ({}));
                throw new Error(errData.message || `Failed to finalize upload (HTTP ${completeResp.status})`);
            }

            this.status = "completed";
            this.emitProgress(totalChunks, totalChunks);
            return true;
        } catch (err: any) {
            this.status = "error";
            this.emitProgress(0, totalChunks, err.message || "Upload error");
            return false;
        }
    }

    public pause() {
        if (this.status === "uploading") {
            this.status = "paused";
        }
    }

    public abort() {
        this.isAborted = true;
        this.status = "idle";
    }
}
