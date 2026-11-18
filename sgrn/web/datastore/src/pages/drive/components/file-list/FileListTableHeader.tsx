import { ArrowDown, ArrowUp } from "lucide-react";
import type { DriveSortKey, DriveSortOrder } from "@/pages/drive/api";
import type { ReactElement } from "react";

interface SortHeaderProps {
    label: string;
    sortKey: DriveSortKey;
    activeSortKey: DriveSortKey;
    sortOrder: DriveSortOrder;
    onSort?: (key: DriveSortKey) => void;
}

function SortHeader({ label, sortKey, activeSortKey, sortOrder, onSort }: SortHeaderProps): ReactElement {
    const active = activeSortKey === sortKey;

    return (
        <th aria-sort={active ? (sortOrder === "asc" ? "ascending" : "descending") : "none"}>
            {onSort ? (
                <button
                    type="button"
                    onClick={(event) => {
                        event.stopPropagation();
                        onSort(sortKey);
                    }}
                    className={`drive-list-head-btn${active ? " drive-list-head-btn--active" : ""}`}
                    aria-label={`Sort by ${label}${active ? (sortOrder === "asc" ? ", currently ascending" : ", currently descending") : ""}`}
                >
                    <span>{label}</span>
                    {active && (sortOrder === "asc" ? <ArrowUp size={10} /> : <ArrowDown size={10} />)}
                </button>
            ) : (
                label
            )}
        </th>
    );
}

interface FileListTableHeaderProps {
    sortBy: DriveSortKey;
    sortOrder: DriveSortOrder;
    onSort?: (key: DriveSortKey) => void;
}

export function FileListTableHeader({ sortBy, sortOrder, onSort }: FileListTableHeaderProps) {
    return (
        <thead>
            <tr>
                <th aria-label="Selection" />
                <th aria-label="Type icon" />
                <SortHeader label="Name" sortKey="name" activeSortKey={sortBy} sortOrder={sortOrder} onSort={onSort} />
                <th>Type</th>
                <SortHeader label="Size" sortKey="size" activeSortKey={sortBy} sortOrder={sortOrder} onSort={onSort} />
                <SortHeader label="Modified" sortKey="modified" activeSortKey={sortBy} sortOrder={sortOrder} onSort={onSort} />
                <th aria-label="Actions" />
            </tr>
        </thead>
    );
}
