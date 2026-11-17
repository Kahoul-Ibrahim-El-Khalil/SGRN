import type { EventType } from "@/contexts/EventContext";
import type { SgrnError, SgrnResult } from "@sgrn/types";
import { isError } from "@sgrn/types";

let global_event_handler: ((t_event_type: EventType, t_message: string, t_scope?: string) => void) | null = null;

export function setGlobalEventHandler(t_handler: ((t_event_type: EventType, t_message: string, t_scope?: string) => void) | null) {
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
