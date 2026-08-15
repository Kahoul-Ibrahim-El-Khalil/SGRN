import type { AutomatedServiceSession, Credentials, SgrnError, SgrnResult, UploadConstraints, UploadResult } from "@sgrn/types";
import { ErrorScope, isError } from "@sgrn/types";

const URLS = {
    sign_in: "/api/v1/auth/automated-service/signin",
    sign_out: "/api/v1/auth/automated-service/signout",
    session: "/api/v1/auth/automated-service/session",
    constraints: "/api/v1/storage/info",
    files: "/api/v1/storage/automated-service/files",
} as const;

// ─────────────────────────────────────────────────────────────────────────────
// Client
// ─────────────────────────────────────────────────────────────────────────────

export class AutomatedServiceClient {
    private credentials: Credentials;
    public session: AutomatedServiceSession | null = null;
    private jwt: string | null = null;
    private base_url: string;

    constructor(t_credentials: Credentials, t_base_url: string) {
        this.credentials = t_credentials;
        this.base_url = t_base_url.replace(/\/$/, "");
    }

    // ── Accessors ────────────────────────────────────────────────────────────

    public setCredentials(t_token: string, t_secret: string) {
        this.credentials = { token: t_token, secret: t_secret };
    }

    public setBaseUrl(t_base_url: string) {
        this.base_url = t_base_url.replace(/\/$/, "");
    }

    public getJwt(): string | null {
        return this.jwt;
    }

    public getCredentials(): Credentials {
        return this.credentials;
    }

    public getBaseUrl(): string {
        return this.base_url;
    }

    // ── Auth ─────────────────────────────────────────────────────────────────

    async signIn(): Promise<SgrnResult<AutomatedServiceSession>> {
        const res = await this.request<{
            token: string;
            automated_service: Record<string, unknown>;
            automated_service_id?: number;
            session_id?: string;
        }>("POST", URLS.sign_in, { token: this.credentials.token, secret: this.credentials.secret }, /* auth */ false);

        if (isError(res)) {
            return res;
        }

        const data = res.data;
        this.jwt = data.token;

        const svc_data = data.automated_service ?? {};
        this.session = {
            automated_service_id: (data.automated_service_id ?? svc_data.id) as number,
            automated_service: svc_data,
            session_id: data.session_id,
        };
        return { data: this.session };
    }

    /**
     * Sign out and clear the local JWT / session state.
     */
    async signOut(): Promise<SgrnResult<void>> {
        const res = await this.request<{ message: string }>("POST", URLS.sign_out);
        if (isError(res)) {
            return res;
        }
        this.jwt = null;
        this.session = null;
        return { data: undefined };
    }

    /**
     * Fetch the current session from the server (also refreshes `this.session`).
     */
    async getSession(): Promise<SgrnResult<AutomatedServiceSession>> {
        const res = await this.request<AutomatedServiceSession>("GET", URLS.session);
        if (isError(res)) {
            return res;
        }
        this.session = res.data;
        return res;
    }

    // ── Storage ──────────────────────────────────────────────────────────────

    /**
     * Retrieve storage constraints (max file size, allowed extensions, etc.).
     * Does not require authentication.
     */
    async getConstraints(): Promise<SgrnResult<UploadConstraints>> {
        return this.request<UploadConstraints>("GET", URLS.constraints, undefined, /* auth */ false);
    }

    /**
     * Download a file at the given virtual path.
     * Returns the raw `Response` so the caller can stream or read as needed.
     *
     * @param t_virtual_path  e.g. "/documents/report.pdf"
     */
    async downloadFile(t_virtual_path: string): Promise<SgrnResult<Response>> {
        try {
            const url = `${this.base_url}${URLS.files}?path=${encodeURIComponent(t_virtual_path)}`;
            const auth_res = this.authHeaders();
            if (isError(auth_res)) {
                return auth_res;
            }

            const res = await fetch(url, { method: "GET", headers: auth_res.data });
            if (!res.ok) {
                return await this.processErrorResponse(res);
            }
            return { data: res };
        } catch (e) {
            return { error: `Network error during download: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
        }
    }

    /**
     * Download a file and return its content as an `ArrayBuffer`.
     *
     * @param t_virtual_path  e.g. "/documents/report.pdf"
     */
    async downloadFileAsBuffer(t_virtual_path: string): Promise<SgrnResult<ArrayBuffer>> {
        const res = await this.downloadFile(t_virtual_path);
        if (isError(res)) {
            return res;
        }
        try {
            const buffer = await res.data.arrayBuffer();
            return { data: buffer };
        } catch (e) {
            return { error: `Failed to read download buffer: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Runtime };
        }
    }

    /**
     * Upload a file to the given virtual path.
     *
     * @param t_file          The `File` or `Blob` to upload.
     * @param t_virtual_path  Destination path, e.g. "/documents/report.pdf".
     */
    async uploadFile(t_file: File | Blob, t_virtual_path: string): Promise<SgrnResult<UploadResult>> {
        try {
            const url = `${this.base_url}${URLS.files}?path=${encodeURIComponent(t_virtual_path)}`;
            const auth_res = this.authHeaders();
            if (isError(auth_res)) {
                return auth_res;
            }

            // Derive a filename so the backend can detect the extension.
            const fallback_name = t_virtual_path.split("/").pop() ?? "upload";
            const file_name = t_file instanceof File ? t_file.name : fallback_name;

            const form_data = new FormData();
            form_data.append("file", t_file, file_name);

            // Render to buffer for compression
            const dummy_req = new Request("http://l", { method: "POST", body: form_data });
            const content_type = dummy_req.headers.get("content-type") || "multipart/form-data";
            const raw_body = await dummy_req.arrayBuffer();

            // Compress using native Bun ZSTD support
            // @ts-ignore: Bun.zstdCompressSync is a Bun-specific extension
            const compressed_body = Bun.zstdCompressSync(new Uint8Array(raw_body));

            const headers = {
                ...auth_res.data,
                "Content-Type": content_type,
                "Content-Encoding": "zstd",
            };

            const res = await fetch(url, { method: "POST", headers, body: compressed_body });
            if (!res.ok) {
                return await this.processErrorResponse(res);
            }
            const data = (await res.json()) as UploadResult;
            return { data };
        } catch (e) {
            return { error: `Upload failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
        }
    }

    /**
     * Upload a file from the local filesystem.
     *
     * @param t_file_path      Absolute or relative path to the file on disk.
     * @param t_virtual_path   Destination path on the server.  Defaults to
     *                         `/<filename>` derived from `t_file_path`.
     */
    async uploadFileFromDisk(t_file_path: string, t_virtual_path?: string): Promise<SgrnResult<UploadResult>> {
        try {
            const bun_file = Bun.file(t_file_path);
            const file_name = t_file_path.split("/").pop() ?? "upload";
            const dest = t_virtual_path ?? `/${file_name}`;

            return this.uploadFile(bun_file, dest);
        } catch (e) {
            return { error: `Failed to read file from disk: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.FileSystem };
        }
    }

    /**
     * Download a file from the server and write it to the local filesystem.
     *
     * @param t_virtual_path  Server path, e.g. "/reports/q2.pdf".
     * @param t_output_path   Local path to write the downloaded content to.
     */
    async downloadFileToDisk(t_virtual_path: string, t_output_path: string): Promise<SgrnResult<void>> {
        const res = await this.downloadFileAsBuffer(t_virtual_path);
        if (isError(res)) {
            return res;
        }
        try {
            await Bun.write(t_output_path, res.data);
            return { data: undefined };
        } catch (e) {
            return { error: `Failed to write file to disk: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.FileSystem };
        }
    }

    // ── Private helpers ────────────────────────────────────────────────────

    private authHeaders(): SgrnResult<Record<string, string>> {
        if (!this.jwt) {
            return { error: "Not signed in – call signIn() first.", scope: ErrorScope.Authentication };
        }
        return { data: { Authorization: `Bearer ${this.jwt}` } };
    }

    private async request<T>(
        t_method: "GET" | "POST" | "PUT" | "PATCH" | "DELETE",
        t_path: string,
        t_body?: unknown,
        t_auth = true,
    ): Promise<SgrnResult<T>> {
        try {
            const url = `${this.base_url}${t_path}`;
            const headers: Record<string, string> = {
                "Content-Type": "application/json",
            };

            if (t_auth) {
                const auth_res = this.authHeaders();
                if (isError(auth_res)) {
                    return auth_res;
                }
                Object.assign(headers, auth_res.data);
            }

            const res = await fetch(url, {
                method: t_method,
                headers,
                body: t_body !== undefined ? JSON.stringify(t_body) : undefined,
            });

            if (!res.ok) {
                return await this.processErrorResponse(res);
            }

            // 204 No Content
            if (res.status === 204) return { data: undefined as unknown as T };

            const data = (await res.json()) as T;
            return { data };
        } catch (e) {
            return { error: `Request failed: ${e instanceof Error ? e.message : String(e)}`, scope: ErrorScope.Network };
        }
    }

    private async processErrorResponse(res: Response): Promise<SgrnResult<any>> {
        const contentType = res.headers.get("Content-Type") || "";

        // Early gateway check
        if (res.status === 502 || res.status === 503 || res.status === 504) {
            return { error: `Server is currently unavailable (${res.status}: ${res.statusText})`, scope: ErrorScope.Network };
        }

        try {
            if (contentType.includes("application/json")) {
                const data = await res.json();
                return {
                    error: data.error || `HTTP ${res.status}: ${res.statusText}`,
                    scope: data.scope || this.mapStatusToScope(res.status),
                };
            } else {
                const text = await res.text().catch(() => res.statusText);
                return { error: text.slice(0, 100) || `HTTP ${res.status}: ${res.statusText}`, scope: this.mapStatusToScope(res.status) };
            }
        } catch (e) {
            return { error: `HTTP ${res.status}: ${res.statusText}`, scope: this.mapStatusToScope(res.status) };
        }
    }

    private mapStatusToScope(status: number): ErrorScope {
        if (status === 401) return ErrorScope.Authentication;
        if (status === 403) return ErrorScope.Authorization;
        if (status >= 502) return ErrorScope.Network;
        return ErrorScope.Unknown;
    }
}
