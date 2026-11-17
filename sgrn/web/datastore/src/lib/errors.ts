// Small helpers for turning thrown values into user-facing messages.

/** Best-effort message for anything that can be thrown. */
export function errorMessage(t_error: unknown, t_fallback = "Unexpected error"): string {
    if (t_error instanceof Error && t_error.message) return t_error.message;
    if (typeof t_error === "string" && t_error) return t_error;
    return t_fallback;
}

/** True for the rejection `fetch` produces when its AbortSignal fires. */
export function isAbortError(t_error: unknown): boolean {
    return t_error instanceof DOMException && t_error.name === "AbortError";
}
