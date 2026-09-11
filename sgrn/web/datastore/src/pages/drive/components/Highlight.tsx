import React from "react";

interface HighlightProps {
    text: string;
    query: string;
}

/**
 * Highlights the occurrences of a query string within a text.
 */
export const Highlight: React.FC<HighlightProps> = ({ text, query }) => {
    if (!query.trim()) {
        return <>{text}</>;
    }

    // Escape regex special characters
    const escapedQuery = query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const parts = text.split(new RegExp(`(${escapedQuery})`, "gi"));

    return (
        <>
            {parts.map((part, i) =>
                part.toLowerCase() === query.toLowerCase() ? (
                    <span
                        key={i}
                        style={{
                            backgroundColor: "rgba(255, 193, 7, 0.4)",
                            color: "#fff",
                            borderRadius: "2px",
                            padding: "0 2px",
                            fontWeight: 600,
                        }}
                    >
                        {part}
                    </span>
                ) : (
                    part
                ),
            )}
        </>
    );
};
