import type { EventType } from "@/contexts/EventContext";
import type { SgrnError, SgrnResult } from "@sgrn/types";
import { isError } from "@sgrn/types";

let global_event_handler: ((t_event_type: EventType, t_message: string, t_scope?: string) => void) | null = null;

// Legacy function for backward compatibility (kept for existing code)
export function setGlobalErrorHandler(t_handler: (t_message: string) => void) {
    global_event_handler = (t_event_type: EventType, t_message: string) => {
        if (t_event_type === "error") {
            t_handler(t_message);
        }
    };
}

// New function that supports all event types
export function setGlobalEventHandler(t_handler: (t_event_type: EventType, t_message: string, t_scope?: string) => void) {
    global_event_handler = t_handler;
}

export function handleApiError(t_error: string | SgrnError) {
    if (global_event_handler) {
        if (typeof t_error === "string") {
            global_event_handler("error", t_error);
        } else {
            global_event_handler("error", t_error.error, t_error.scope);
        }
    } else {
        console.error("No event handler registered:", t_error);
    }
}

/**
 * Convenience helper to handle SgrnResult
 */
export function handleSgrnResult<T>(t_result: SgrnResult<T>): t_result is { data: T } {
    if (isError(t_result)) {
        handleApiError(t_result);
        return false;
    }
    return true;
}

export function handleApiSuccess(t_message: string) {
    if (global_event_handler) {
        global_event_handler("success", t_message);
    } else {
        console.log("No event handler registered:", t_message);
    }
}

export function handleApiInfo(t_message: string) {
    if (global_event_handler) {
        global_event_handler("info", t_message);
    } else {
        console.info("No event handler registered:", t_message);
    }
}

export function handleApiWarning(t_message: string) {
    if (global_event_handler) {
        global_event_handler("warning", t_message);
    } else {
        console.warn("No event handler registered:", t_message);
    }
}
