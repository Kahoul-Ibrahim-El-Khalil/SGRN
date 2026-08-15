// @/components/workspace/download/FileSizeDisplay.tsx

interface SizeDisplayProps {
    size: number;
}

export function SizeDisplay({ size }: SizeDisplayProps) {
    const formatFileSize = (bytes: number): string => {
        const mb = bytes / (1024 * 1024);
        return mb >= 1 ? `${mb.toFixed(2)} MB` : `${(bytes / 1024).toFixed(2)} KB`;
    };

    return <span>{formatFileSize(size)}</span>;
}
