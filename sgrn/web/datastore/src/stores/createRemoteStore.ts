// createRemoteStore — a tiny shared cache for server data that more than one
// panel needs (the domain list is used by four of them).
//
// Before this, every panel fetched its own copy: creating a domain in one tab
// left the user editor's domain dropdown stale until it happened to reload.
// With a store there is one copy, one in-flight request, and every subscribed
// component re-renders when it changes.
//
// It plugs into React through useSyncExternalStore, so there is no Context
// provider to mount and no extra render pass.
import { useEffect, useRef, useSyncExternalStore } from "react";
import { errorMessage } from "@/lib/errors";
import type { AsyncStatus } from "@/hooks/useAsyncResource";

export interface RemoteSnapshot<T> {
    readonly data: T;
    readonly status: AsyncStatus;
    readonly error: string | null;
}

export interface RemoteStore<T> {
    getSnapshot: () => RemoteSnapshot<T>;
    subscribe: (t_listener: () => void) => () => void;
    /** Load now. The newest call wins; a slower, older response is dropped. */
    refresh: () => Promise<void>;
    /** Load only if nothing has been loaded yet (no-op while loading or ready). */
    ensureLoaded: () => Promise<void>;
    /** Local edit of the cached value (optimistic updates). */
    set: (t_next: T | ((t_prev: T) => T)) => void;
    /** Back to the empty state — call on sign-in / sign-out. */
    reset: () => void;
}

export function createRemoteStore<T>(t_initial: T, t_load: () => Promise<T>): RemoteStore<T> {
    const idle: RemoteSnapshot<T> = { data: t_initial, status: "idle", error: null };
    let snapshot = idle;
    let generation = 0;
    const listeners = new Set<() => void>();

    const publish = (t_next: RemoteSnapshot<T>) => {
        snapshot = t_next;
        listeners.forEach((l) => l());
    };

    const refresh = async (): Promise<void> => {
        const own = ++generation;
        publish({ data: snapshot.data, status: "loading", error: null });
        try {
            const data = await t_load();
            if (own !== generation) return;
            publish({ data, status: "ready", error: null });
        } catch (err) {
            if (own !== generation) return;
            publish({ data: snapshot.data, status: "error", error: errorMessage(err, "Request failed") });
        }
    };

    return {
        getSnapshot: () => snapshot,
        subscribe: (t_listener) => {
            listeners.add(t_listener);
            return () => {
                listeners.delete(t_listener);
            };
        },
        refresh,
        ensureLoaded: () => (snapshot.status === "idle" ? refresh() : Promise.resolve()),
        set: (t_next) => {
            const data = typeof t_next === "function" ? (t_next as (t_prev: T) => T)(snapshot.data) : t_next;
            publish({ ...snapshot, data });
        },
        reset: () => {
            generation++; // invalidate anything still in flight
            publish(idle);
        },
    };
}

export interface RemoteStoreView<T> extends RemoteSnapshot<T> {
    loading: boolean;
    refresh: () => Promise<void>;
    set: RemoteStore<T>["set"];
}

/** Subscribe a component to a store; loads it on first use. */
export function useRemoteStore<T>(t_store: RemoteStore<T>, t_onError?: (t_message: string) => void): RemoteStoreView<T> {
    const snapshot = useSyncExternalStore(t_store.subscribe, t_store.getSnapshot, t_store.getSnapshot);

    useEffect(() => {
        void t_store.ensureLoaded();
    }, [t_store]);

    // Surface each distinct failure once, not on every re-render.
    const reported = useRef<string | null>(null);
    const onErrorRef = useRef(t_onError);
    useEffect(() => {
        onErrorRef.current = t_onError;
    });
    useEffect(() => {
        if (snapshot.error === null) {
            reported.current = null;
        } else if (snapshot.error !== reported.current) {
            reported.current = snapshot.error;
            onErrorRef.current?.(snapshot.error);
        }
    }, [snapshot.error]);

    return { ...snapshot, loading: snapshot.status === "loading", refresh: t_store.refresh, set: t_store.set };
}
