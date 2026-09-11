// @/components/EventHandlerInit.tsx
import { useEffect } from "react";
import { useEvent } from "@/contexts/EventContext";
import { setGlobalEventHandler } from "@/backend/errorHandler";
/**
 * Component that connects the global error handler to the Event context
 * Must be rendered inside EventProvider
 */
export function EventHandlerInit() {
    const { showEvent } = useEvent();

    useEffect(() => {
        setGlobalEventHandler(showEvent);
        return () => setGlobalEventHandler(() => {});
    }, [showEvent]);

    return null;
}
