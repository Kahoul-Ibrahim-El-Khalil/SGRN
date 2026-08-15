import { useState, useEffect } from "react";

export function useClock() {
    const [time, setTime] = useState(new Date());
    useEffect(() => {
        const timer = setInterval(() => setTime(new Date()), 1000);
        return () => clearInterval(timer);
    }, []);
    return {
        timeStr: time.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }),
        dateStr: time.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric", year: "numeric" }),
    };
}
