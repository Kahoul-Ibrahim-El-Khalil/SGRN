// useAsyncResource — load / loading / error / reload for one remote value.
//
// Replaces the `[data, setData] + [loading, setLoading]` pair and the
// hand-rolled try/catch/finally around every list fetch. One reducer keeps the
// three values consistent, and a request counter drops responses that arrive
// after a newer reload (or after unmount) instead of letting them overwrite
// fresh data.
import { useCallback, useEffect, useReducer, useRef } from "react";
import { errorMessage, isAbortError } from "@/lib/errors";

export type AsyncStatus = "idle" | "loading" | "ready" | "error";

export interface AsyncState<T> {
    readonly data: T;
    readonly status: AsyncStatus;
    readonly error: string | null;
}

type AsyncAction<T> =
    { type: "start" } | { type: "success"; data: T } | { type: "failure"; message: string } | { type: "update"; update: (t_prev: T) => T };

function asyncReducer<T>(t_state: AsyncState<T>, t_action: AsyncAction<T>): AsyncState<T> {
    switch (t_action.type) {
        case "start":
            // Keep the previous data on screen while reloading.
            return { data: t_state.data, status: "loading", error: null };
        case "success":
            return { data: t_action.data, status: "ready", error: null };
        case "failure":
            return { data: t_state.data, status: "error", error: t_action.message };
        case "update":
            return { ...t_state, data: t_action.update(t_state.data) };
    }
}

export interface AsyncResource<T> extends AsyncState<T> {
    loading: boolean;
    reload: () => Promise<void>;
    /** Optimistic / local edit of the loaded value. */
    setData: (t_next: T | ((t_prev: T) => T)) => void;
}

export interface AsyncResourceOptions {
    /** Called once per failed load with a printable message. */
    onError?: (t_message: string) => void;
    /** Load on mount (default). Pass false to load only on `reload()`. */
    auto?: boolean;
}

export function useAsyncResource<T>(
    t_load: (t_signal: AbortSignal) => Promise<T>,
    t_initial: T,
    t_options: AsyncResourceOptions = {},
): AsyncResource<T> {
    const { onError, auto = true } = t_options;
    const [state, dispatch] = useReducer(asyncReducer<T>, { data: t_initial, status: "idle", error: null });

    // Latest callbacks without making `reload` change identity every render.
    const loadRef = useRef(t_load);
    const onErrorRef = useRef(onError);
    useEffect(() => {
        loadRef.current = t_load;
        onErrorRef.current = onError;
    });

    const requestId = useRef(0);
    const controller = useRef<AbortController | null>(null);

    const reload = useCallback(async () => {
        controller.current?.abort();
        const own = new AbortController();
        controller.current = own;
        const id = ++requestId.current;
        dispatch({ type: "start" });
        try {
            const data = await loadRef.current(own.signal);
            if (id !== requestId.current) return;
            dispatch({ type: "success", data });
        } catch (err) {
            if (id !== requestId.current || isAbortError(err)) return;
            const message = errorMessage(err, "Request failed");
            dispatch({ type: "failure", message });
            onErrorRef.current?.(message);
        }
    }, []);

    useEffect(() => {
        if (auto) void reload();
        return () => {
            requestId.current++;
            controller.current?.abort();
        };
    }, [auto, reload]);

    const setData = useCallback((t_next: T | ((t_prev: T) => T)) => {
        dispatch({ type: "update", update: typeof t_next === "function" ? (t_next as (t_prev: T) => T) : () => t_next });
    }, []);

    return { ...state, loading: state.status === "loading", reload, setData };
}
