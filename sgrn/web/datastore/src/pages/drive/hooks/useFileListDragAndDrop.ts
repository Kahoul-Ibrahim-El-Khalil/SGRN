import { useCallback, useState } from "react";
import type { DragEvent } from "react";
import type { DriveFolder } from "@sgrn/types";
import type { DriveMoveItem } from "@/pages/drive/types";
import { readDriveMoveItem, writeDriveMoveItem } from "@/pages/drive/dragAndDrop";

const INTERNAL_DRAG_TYPES = new Set(["application/x-sgrn-drive-item+json", "application/json"]);

export function useFileListDragAndDrop(onMoveItems?: (items: DriveMoveItem[], targetId: number) => void) {
    const [dragOverPath, setDragOverPath] = useState<string | null>(null);

    const handleDragStart = useCallback(
        (event: DragEvent<HTMLElement>, item: DriveMoveItem) => {
            if (!onMoveItems) return;

            event.dataTransfer.effectAllowed = "move";
            writeDriveMoveItem(event.dataTransfer, item);
        },
        [onMoveItems],
    );

    const handleDragEnd = useCallback(() => {
        setDragOverPath(null);
    }, []);

    const handleFolderDragOver = useCallback(
        (event: DragEvent<HTMLElement>, folder: DriveFolder) => {
            if (!onMoveItems || folder.id == null) return;

            const isInternalMove = Array.from(event.dataTransfer.types).some((type) => INTERNAL_DRAG_TYPES.has(type));
            if (!isInternalMove) return;

            event.preventDefault();
            event.dataTransfer.dropEffect = "move";
            setDragOverPath(folder.path);
        },
        [onMoveItems],
    );

    const handleFolderDragLeave = useCallback((event: DragEvent<HTMLElement>, folderPath: string) => {
        const nextTarget = event.relatedTarget;
        if (typeof Node !== "undefined" && nextTarget instanceof Node && event.currentTarget.contains(nextTarget)) return;

        setDragOverPath((currentPath) => (currentPath === folderPath ? null : currentPath));
    }, []);

    const handleFolderDrop = useCallback(
        (event: DragEvent<HTMLElement>, targetFolder: DriveFolder) => {
            setDragOverPath(null);
            if (!onMoveItems || targetFolder.id == null) return;

            const draggedItem = readDriveMoveItem(event.dataTransfer);
            // Let the page-level upload drop target handle files dragged from the OS.
            if (!draggedItem) return;

            event.preventDefault();
            event.stopPropagation();

            // A folder cannot be moved into itself.
            if (draggedItem.type === "folder" && draggedItem.id === targetFolder.id) return;

            onMoveItems([draggedItem], targetFolder.id);
        },
        [onMoveItems],
    );

    return {
        dragOverPath,
        handleDragStart,
        handleDragEnd,
        handleFolderDragOver,
        handleFolderDragLeave,
        handleFolderDrop,
    };
}
