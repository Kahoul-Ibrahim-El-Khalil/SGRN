// @/contexts/EventContext.tsx
import React, { createContext, useContext, useState, useCallback, useMemo, useRef, useEffect } from "react";

export type EventType = "success" | "error" | "info" | "warning";

interface Event {
    id: string;
    type: EventType;
    message: string;
    scope?: string;
    timestamp: number;
}

interface EventContextType {
    events: Event[];
    showEvent: (t_type: EventType, t_message: string, t_scope?: string) => void;
    clearEvent: (t_id: string) => void;
    clearAllEvents: () => void;
}

const EventContext = createContext<EventContextType | undefined>(undefined);

const AUTO_DISMISS_MS = 5000;

export function EventProvider({ children }: { children: React.ReactNode }) {
    const [events, setEvents] = useState<Event[]>([]);
    // Track pending auto-dismiss timers so they can be cleared on unmount.
    const timers = useRef<Set<number>>(new Set());

    useEffect(() => {
        const pending = timers.current;
        return () => {
            pending.forEach((timer) => window.clearTimeout(timer));
            pending.clear();
        };
    }, []);

    const showEvent = useCallback((t_type: EventType, t_message: string, t_scope?: string) => {
        const id = `${Date.now()}-${Math.random()}`;
        const newEvent: Event = {
            id,
            type: t_type,
            message: t_message,
            scope: t_scope,
            timestamp: Date.now(),
        };

        setEvents((prev) => [...prev, newEvent]);

        const timer = window.setTimeout(() => {
            setEvents((prev) => prev.filter((e) => e.id !== id));
            timers.current.delete(timer);
        }, AUTO_DISMISS_MS);
        timers.current.add(timer);
    }, []);

    const clearEvent = useCallback((t_id: string) => {
        setEvents((prev) => prev.filter((e) => e.id !== t_id));
    }, []);

    const clearAllEvents = useCallback(() => {
        setEvents([]);
    }, []);

    // Memoized so consumers don't re-render when unrelated state changes.
    const value = useMemo<EventContextType>(
        () => ({ events, showEvent, clearEvent, clearAllEvents }),
        [events, showEvent, clearEvent, clearAllEvents],
    );

    return <EventContext.Provider value={value}>{children}</EventContext.Provider>;
}

export function useEvent() {
    const context = useContext(EventContext);
    if (!context) {
        throw new Error("useEvent must be used within EventProvider");
    }
    return context;
}
