// @/components/EventNotification.tsx
import { X, CheckCircle, XCircle, Info, AlertTriangle } from "lucide-react";
import { useEvent } from "@/contexts/EventContext";
import type { EventType } from "@/contexts/EventContext";
const eventConfig: Record<EventType, { icon: React.ElementType; colors: string }> = {
    success: {
        icon: CheckCircle,
        colors: "bg-green-50 dark:bg-green-900/30 border-green-500 text-green-800 dark:text-green-200",
    },
    error: {
        icon: XCircle,
        colors: "bg-red-50 dark:bg-red-900/30 border-red-500 text-red-800 dark:text-red-200",
    },
    info: {
        icon: Info,
        colors: "bg-blue-50 dark:bg-blue-900/30 border-blue-500 text-blue-800 dark:text-blue-200",
    },
    warning: {
        icon: AlertTriangle,
        colors: "bg-yellow-50 dark:bg-yellow-900/30 border-yellow-500 text-yellow-800 dark:text-yellow-200",
    },
};

export function EventNotification() {
    const { events, clearEvent } = useEvent();

    if (events.length === 0) return null;

    return (
        <div className="notification-container">
            {events.map((event) => {
                const config = eventConfig[event.type];
                const Icon = config.icon;

                return (
                    <div key={event.id} className={`notification-item ${config.colors}`}>
                        <div className="flex items-start gap-3">
                            <div className="flex-shrink-0">
                                <Icon size={18} />
                            </div>
                            <div className="flex-1 min-w-0">
                                <div className="flex items-center gap-2">
                                    <h3 className="text-[10px] font-black uppercase tracking-widest">{event.type}</h3>
                                    {event.scope && <span className="notification-scope">{event.scope}</span>}
                                </div>
                                <p className="mt-1 text-xs font-medium leading-relaxed opacity-90">{event.message}</p>
                            </div>
                            <button
                                onClick={() => clearEvent(event.id)}
                                className="flex-shrink-0 opacity-50 hover:opacity-100 transition-opacity"
                                aria-label="Close"
                            >
                                <X size={16} />
                            </button>
                        </div>
                    </div>
                );
            })}
        </div>
    );
}
