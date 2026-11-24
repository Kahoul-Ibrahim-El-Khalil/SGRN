import { useCallback, useEffect, useRef, useState } from "react";
import type { DriveFile } from "@sgrn/types";
import type { StorageScope } from "@/pages/drive/api";
import { listFileVersions, pruneFileVersions, restoreFileVersion, downloadFileVersion, type FileVersion } from "@/pages/drive/versioning";

export interface FileVersionHistoryState {
    versions: FileVersion[];
    loading: boolean;
    busyVersionNo: number | null;
    error: string | null;
    keepInput: number;
}

const INITIAL_STATE: FileVersionHistoryState = {
    versions: [],
    loading: true,
    busyVersionNo: null,
    error: null,
    keepInput: 10,
};

/** Clamp a retention input to a whole number of versions, never below 1. */
export function normalizeKeepCount(value: number): number {
    return Math.max(1, Math.floor(Number.isFinite(value) ? value : 1));
}

interface UseFileVersionHistoryOptions {
    file: DriveFile;
    scope: StorageScope;
    onChanged?: () => void;
}

export function useFileVersionHistory({ file, scope, onChanged }: UseFileVersionHistoryOptions) {
    const [state, setState] = useState<FileVersionHistoryState>(INITIAL_STATE);
    const requestGeneration = useRef(0);
    const mounted = useRef(false);
    const fileIdentity = `${file.id}:${file.path}:${scope}`;
    const activeFileIdentity = useRef(fileIdentity);

    const patch = useCallback((next: Partial<FileVersionHistoryState>) => {
        if (mounted.current) setState((current) => ({ ...current, ...next }));
    }, []);

    const reload = useCallback(async (): Promise<void> => {
        const generation = ++requestGeneration.current;
        patch({ loading: true, error: null });

        try {
            const result = await listFileVersions(file.path, scope);
            if (generation !== requestGeneration.current || activeFileIdentity.current !== fileIdentity) return;
            if ("error" in result) {
                patch({ loading: false, versions: [], error: result.error || "Unable to load version history." });
            } else {
                patch({ loading: false, versions: result.data });
            }
        } catch (error: unknown) {
            if (generation !== requestGeneration.current || activeFileIdentity.current !== fileIdentity) return;
            patch({
                loading: false,
                versions: [],
                error: error instanceof Error ? error.message : "Unable to load version history.",
            });
        }
    }, [file.path, fileIdentity, scope, patch]);

    useEffect(() => {
        mounted.current = true;
        activeFileIdentity.current = fileIdentity;
        patch({ ...INITIAL_STATE });
        void reload();

        return () => {
            mounted.current = false;
            requestGeneration.current += 1;
        };
    }, [fileIdentity, patch, reload]);

    const restore = useCallback(
        async (version: FileVersion): Promise<void> => {
            const identity = fileIdentity;
            patch({ busyVersionNo: version.version_no, error: null });
            try {
                const result = await restoreFileVersion(file.path, version.version_no, scope);
                if (activeFileIdentity.current !== identity) return;
                if ("error" in result) {
                    patch({ error: result.error || "Failed to restore version." });
                    return;
                }
                await reload();
                if (activeFileIdentity.current === identity) onChanged?.();
            } catch (error: unknown) {
                if (activeFileIdentity.current === identity) {
                    patch({ error: error instanceof Error ? error.message : "Failed to restore version." });
                }
            } finally {
                if (activeFileIdentity.current === identity) patch({ busyVersionNo: null });
            }
        },
        [file.path, fileIdentity, scope, patch, reload, onChanged],
    );

    const download = useCallback(
        async (version: FileVersion): Promise<void> => {
            const identity = fileIdentity;
            patch({ busyVersionNo: version.version_no, error: null });
            try {
                const result = await downloadFileVersion(file.path, file.name, version.version_no, scope);
                if (activeFileIdentity.current === identity && "error" in result) {
                    patch({ error: result.error || "Failed to download version." });
                }
            } catch (error: unknown) {
                if (activeFileIdentity.current === identity) {
                    patch({ error: error instanceof Error ? error.message : "Failed to download version." });
                }
            } finally {
                if (activeFileIdentity.current === identity) patch({ busyVersionNo: null });
            }
        },
        [file.path, file.name, fileIdentity, scope, patch],
    );

    const prune = useCallback(async (): Promise<void> => {
        const identity = fileIdentity;
        const keep = normalizeKeepCount(state.keepInput);
        patch({ busyVersionNo: -1, error: null });
        try {
            const result = await pruneFileVersions(file.path, keep, scope);
            if (activeFileIdentity.current !== identity) return;
            if ("error" in result) {
                patch({ error: result.error || "Failed to prune version history." });
                return;
            }
            await reload();
            if (activeFileIdentity.current === identity) onChanged?.();
        } catch (error: unknown) {
            if (activeFileIdentity.current === identity) {
                patch({ error: error instanceof Error ? error.message : "Failed to prune version history." });
            }
        } finally {
            if (activeFileIdentity.current === identity) patch({ busyVersionNo: null });
        }
    }, [state.keepInput, file.path, fileIdentity, scope, patch, reload, onChanged]);

    const setKeepInput = useCallback((keepInput: number) => patch({ keepInput }), [patch]);

    return { state, reload, restore, download, prune, setKeepInput };
}
