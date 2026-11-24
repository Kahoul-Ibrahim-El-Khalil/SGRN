// @/pages/drive/versioning.ts
//
// Typed datastore boundary for Drive file-version operations. Components own
// presentation state only; uploads, validation, and version-history requests
// stay in this module.

import { authenticatedFetch, readBody } from "@/backend/api/fetcher";
import { StorageBackendApiEndpoints } from "@/backend/endpoints";
import type { DriveFile, SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";
import { uploadDriveFile, uploadResumableDriveFile, type DriveUploadReceipt, type StorageScope } from "@/pages/drive/api";

/** Wire contract returned by GET /api/v1/storage/info (matches datastore StorageConstraints). */
export interface StorageConstraintsResponse {
    max_file_size_mb: number;
    max_file_size_bytes: number;
    encryption_supported: boolean;
    /** Whether the datastore keeps file versions. Absent on older datastore builds. */
    versioning_enabled?: boolean;
}

/** Stable DTO returned by GET /api/v1/storage/files/versions. */
export interface FileVersion {
    version_id: number;
    file_id: number;
    version_no: number;
    storage_kind: "full" | "delta";
    is_checkpoint: boolean;
    content_sha512: string;
    original_size: number;
    is_encrypted: boolean;
    comment: string | null;
    restored_from_version_id: number | null;
    created_by_user_id: number | null;
    created_by_service_id: number | null;
    created_at: string;
    is_current: boolean;
    /** Bytes actually stored for this version (full copy or delta blob). */
    stored_size: number;
}

export interface DriveVersionUploadReceipt extends DriveUploadReceipt {
    is_encrypted?: boolean;
}

const stripLeadingSlash = (t_path: string): string => (t_path.startsWith("/") ? t_path.slice(1) : t_path);

const networkError = <T>(t_operation: string, t_error: unknown): SgrnResult<T> => ({
    error: `${t_operation}: ${t_error instanceof Error ? t_error.message : String(t_error)}`,
    scope: ErrorScope.Network,
});

function isFileVersion(t_value: unknown): t_value is FileVersion {
    if (typeof t_value !== "object" || t_value === null) return false;
    const row = t_value as Record<string, unknown>;
    return (
        typeof row.version_id === "number" &&
        Number.isSafeInteger(row.version_id) &&
        typeof row.file_id === "number" &&
        Number.isSafeInteger(row.file_id) &&
        typeof row.version_no === "number" &&
        Number.isSafeInteger(row.version_no) &&
        row.version_no > 0 &&
        (row.storage_kind === "full" || row.storage_kind === "delta") &&
        typeof row.is_checkpoint === "boolean" &&
        typeof row.content_sha512 === "string" &&
        typeof row.original_size === "number" &&
        Number.isFinite(row.original_size) &&
        row.original_size >= 0 &&
        typeof row.is_encrypted === "boolean" &&
        (typeof row.comment === "string" || row.comment === null) &&
        (row.restored_from_version_id === null ||
            (typeof row.restored_from_version_id === "number" && Number.isSafeInteger(row.restored_from_version_id))) &&
        (row.created_by_user_id === null || (typeof row.created_by_user_id === "number" && Number.isSafeInteger(row.created_by_user_id))) &&
        (row.created_by_service_id === null ||
            (typeof row.created_by_service_id === "number" && Number.isSafeInteger(row.created_by_service_id))) &&
        typeof row.created_at === "string" &&
        typeof row.is_current === "boolean" &&
        typeof row.stored_size === "number" &&
        Number.isFinite(row.stored_size) &&
        row.stored_size >= 0
    );
}

/** Read datastore upload limits before sending replacement bytes. */
export async function getStorageConstraints(): Promise<SgrnResult<StorageConstraintsResponse>> {
    try {
        const response = await authenticatedFetch(StorageBackendApiEndpoints.CONSTRAINTS);
        const data = await readBody<Partial<StorageConstraintsResponse> & { error?: string; scope?: ErrorScope }>(response, {});
        if (!response.ok) {
            return {
                error: data.error || `Failed to read storage capabilities: ${response.status}`,
                scope: data.scope || ErrorScope.Unknown,
            };
        }
        if (
            typeof data.max_file_size_mb !== "number" ||
            typeof data.max_file_size_bytes !== "number" ||
            typeof data.encryption_supported !== "boolean"
        ) {
            return {
                error: "The datastore returned an incomplete storage-capabilities response.",
                scope: ErrorScope.Unknown,
            };
        }
        return { data: data as StorageConstraintsResponse };
    } catch (error) {
        return networkError("Failed to read storage capabilities", error);
    }
}

export async function listFileVersions(t_path: string, t_scope: StorageScope = "personal"): Promise<SgrnResult<FileVersion[]>> {
    try {
        const params = new URLSearchParams({ path: stripLeadingSlash(t_path), scope: t_scope });
        const response = await authenticatedFetch(`${StorageBackendApiEndpoints.VERSIONS}?${params.toString()}`);
        const data = await readBody<unknown>(response, null);
        if (!response.ok) {
            const error = data as { error?: string; scope?: ErrorScope } | null;
            return {
                error: error?.error || `Failed to list versions: ${response.status}`,
                scope: error?.scope || ErrorScope.Unknown,
            };
        }
        if (!Array.isArray(data) || !data.every(isFileVersion)) {
            return { error: "The datastore returned an invalid file-version list.", scope: ErrorScope.Unknown };
        }
        return { data: data as FileVersion[] };
    } catch (error) {
        return networkError("Failed to list versions", error);
    }
}

export async function downloadFileVersion(
    t_path: string,
    t_name: string,
    t_version_no: number,
    t_scope: StorageScope = "personal",
): Promise<SgrnResult<void>> {
    try {
        if (!Number.isSafeInteger(t_version_no) || t_version_no < 1) {
            return { error: "Version number must be a positive integer.", scope: ErrorScope.Unknown };
        }
        const params = new URLSearchParams({ path: stripLeadingSlash(t_path), scope: t_scope, version: String(t_version_no) });
        const response = await authenticatedFetch(`${StorageBackendApiEndpoints.DOWNLOAD}?${params.toString()}`);
        if (!response.ok) {
            const error = await readBody<{ error?: string; scope?: ErrorScope }>(response, {});
            return { error: error.error || `Download failed: ${response.status}`, scope: error.scope || ErrorScope.Network };
        }

        const blob = await response.blob();
        const dot = t_name.lastIndexOf(".");
        const versionedName = dot > 0 ? `${t_name.slice(0, dot)}.v${t_version_no}${t_name.slice(dot)}` : `${t_name}.v${t_version_no}`;
        const url = URL.createObjectURL(blob);
        const anchor = document.createElement("a");
        anchor.href = url;
        anchor.download = versionedName;
        document.body.appendChild(anchor);
        anchor.click();
        document.body.removeChild(anchor);
        window.setTimeout(() => URL.revokeObjectURL(url), 1000);
        return { data: undefined };
    } catch (error) {
        return networkError("Download failed", error);
    }
}

export async function restoreFileVersion(
    t_path: string,
    t_version_no: number,
    t_scope: StorageScope = "personal",
    t_comment?: string,
): Promise<SgrnResult<{ version_id: number; version_no: number; created: boolean }>> {
    try {
        if (!Number.isSafeInteger(t_version_no) || t_version_no < 1) {
            return { error: "Version number must be a positive integer.", scope: ErrorScope.Unknown };
        }
        const response = await authenticatedFetch(`${StorageBackendApiEndpoints.VERSIONS}/restore`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ path: stripLeadingSlash(t_path), scope: t_scope, version_no: t_version_no, comment: t_comment }),
        });
        const data = await readBody<{ error?: string; scope?: ErrorScope; version_id?: number; version_no?: number; created?: boolean }>(
            response,
            {},
        );
        if (!response.ok) return { error: data.error || `Restore failed: ${response.status}`, scope: data.scope || ErrorScope.Unknown };
        if (
            typeof data.version_id !== "number" ||
            !Number.isSafeInteger(data.version_id) ||
            typeof data.version_no !== "number" ||
            !Number.isSafeInteger(data.version_no) ||
            typeof data.created !== "boolean"
        ) {
            return { error: "The datastore returned an invalid restore receipt.", scope: ErrorScope.Unknown };
        }
        return { data: { version_id: data.version_id, version_no: data.version_no, created: data.created } };
    } catch (error) {
        return networkError("Restore failed", error);
    }
}

export async function pruneFileVersions(
    t_path: string,
    t_keep: number,
    t_scope: StorageScope = "personal",
): Promise<SgrnResult<{ removed: number }>> {
    try {
        if (!Number.isSafeInteger(t_keep) || t_keep < 1) {
            return { error: "The number of retained versions must be a positive integer.", scope: ErrorScope.Unknown };
        }
        const params = new URLSearchParams({ path: stripLeadingSlash(t_path), scope: t_scope, keep: String(t_keep) });
        const response = await authenticatedFetch(`${StorageBackendApiEndpoints.VERSIONS}?${params.toString()}`, { method: "DELETE" });
        const data = await readBody<{ error?: string; scope?: ErrorScope; removed?: number }>(response, {});
        if (!response.ok) return { error: data.error || `Prune failed: ${response.status}`, scope: data.scope || ErrorScope.Unknown };
        if (typeof data.removed !== "number" || !Number.isSafeInteger(data.removed) || data.removed < 0) {
            return { error: "The datastore returned an invalid prune receipt.", scope: ErrorScope.Unknown };
        }
        return { data: { removed: data.removed } };
    } catch (error) {
        return networkError("Prune failed", error);
    }
}

function getParentDirectoryPath(t_file: Pick<DriveFile, "path" | "name">): string | null {
    const normalizedPath = t_file.path.replace(/\/+$/, "");
    const separator = normalizedPath.lastIndexOf("/");
    const basename = normalizedPath.slice(separator + 1);
    if (!normalizedPath || basename !== t_file.name) return null;
    if (separator <= 0) return "/";
    return normalizedPath.slice(0, separator);
}

/**
 * Upload replacement bytes to the existing file's exact path. The datastore's
 * storage pipeline owns head snapshotting and version commit; this API only
 * preserves the logical path and selects the same direct/resumable transport
 * used by ordinary Drive uploads.
 */
export async function uploadDriveFileVersion(
    t_existing_file: DriveFile,
    t_replacement_file: File,
    t_scope: StorageScope = "personal",
    t_on_progress?: (percent: number) => void,
    t_encrypt: boolean = false,
): Promise<SgrnResult<DriveVersionUploadReceipt>> {
    const directoryPath = getParentDirectoryPath(t_existing_file);
    if (!directoryPath) {
        return { error: "Cannot determine the existing file's parent directory; no upload was started.", scope: ErrorScope.Unknown };
    }

    const constraints = await getStorageConstraints();
    if ("error" in constraints) {
        return {
            error: constraints.error || "Unable to read datastore storage capabilities.",
            scope: constraints.scope ?? ErrorScope.Unknown,
        };
    }
    if (constraints.data.max_file_size_bytes > 0 && t_replacement_file.size > constraints.data.max_file_size_bytes) {
        return {
            error: `The selected file exceeds the datastore limit of ${constraints.data.max_file_size_mb} MiB.`,
            scope: ErrorScope.Unknown,
        };
    }

    try {
        // Preserve the existing file's encryption by default. The global toggle
        // can opt into encryption, but should not silently downgrade an
        // encrypted logical file to plaintext.
        const encryptReplacement = t_encrypt || t_existing_file.is_encrypted;

        // The selected bytes replace the content, not the logical file name.
        const replacement = new File([t_replacement_file], t_existing_file.name, {
            type: t_replacement_file.type || "application/octet-stream",
            lastModified: t_replacement_file.lastModified,
        });

        // Match ordinary Drive transport policy. Version creation itself is
        // governed by the datastore's versioning configuration and upload path;
        // /storage/info exposes size, encryption support and whether versioning is enabled.
        // The server remains authoritative for scope and policy enforcement.
        const useResumable = replacement.size >= 5 * 1024 * 1024 || encryptReplacement || t_scope === "domains";
        if (useResumable) {
            const result = await uploadResumableDriveFile(
                directoryPath,
                replacement,
                t_scope,
                (progress) => t_on_progress?.(progress.percent),
                encryptReplacement,
            );
            if ("error" in result) {
                return {
                    error: result.error || "Resumable version upload failed.",
                    scope: result.scope ?? ErrorScope.Network,
                };
            }
            return { data: { is_encrypted: result.data?.is_encrypted } };
        }

        return await uploadDriveFile(directoryPath, replacement, t_scope, t_on_progress);
    } catch (error) {
        return networkError("Version upload failed", error);
    }
}
