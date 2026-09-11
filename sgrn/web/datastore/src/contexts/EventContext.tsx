// @/contexts/EventContext.tsx
import React, { createContext, useContext, useState, useCallback } from "react";

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

export function EventProvider({ children }: { children: React.ReactNode }) {
    const [events, setEvents] = useState<Event[]>([]);

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

        // Auto-dismiss after 5 seconds
        setTimeout(() => {
            setEvents((prev) => prev.filter((e) => e.id !== id));
        }, 5000);
    }, []);

    const clearEvent = useCallback((t_id: string) => {
        setEvents((prev) => prev.filter((e) => e.id !== t_id));
    }, []);

    const clearAllEvents = useCallback(() => {
        setEvents([]);
    }, []);

    return <EventContext.Provider value={{ events, showEvent, clearEvent, clearAllEvents }}>{children}</EventContext.Provider>;
}

export function useEvent() {
    const context = useContext(EventContext);
    if (!context) {
        throw new Error("useEvent must be used within EventProvider");
    }
    return context;
}
