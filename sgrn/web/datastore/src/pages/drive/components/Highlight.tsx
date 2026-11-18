import React from "react";

interface HighlightProps {
    text: string;
    query: string;
}

/**
 * Highlights the occurrences of a query string within a text.
 *
 * Uses a theme-aware CSS class instead of hard-coded colours so it stays
 * readable in both light and dark themes.
 */
export const Highlight = React.memo(function Highlight({ text, query }: HighlightProps) {
    if (!query.trim()) {
        return <>{text}</>;
    }

    // Escape regex special characters
    const escapedQuery = query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const parts = text.split(new RegExp(`(${escapedQuery})`, "gi"));
    const lowerQuery = query.toLowerCase();

    return (
        <>
            {parts.map((part, i) =>
                part.toLowerCase() === lowerQuery ? (
                    <mark key={i} className="drive-highlight">
                        {part}
                    </mark>
                ) : (
                    part
                ),
            )}
        </>
    );
});
