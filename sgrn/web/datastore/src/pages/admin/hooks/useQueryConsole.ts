// State and actions of the admin "query console": pick a table, type a
// `column=op.value` filter string, run a GET, show whatever came back.
import { useState } from "react";
import { authenticatedFetch, readBody } from "@/backend/api/fetcher";
import { useEvent } from "@/contexts/EventContext";
import { useFormState } from "@/hooks/useFormState";
import { usePending } from "@/hooks/usePending";
import { errorMessage } from "@/lib/errors";

/** Where each console "table" is served. */
function endpointFor(t_table: string): string {
    switch (t_table) {
        // `organisations` is served by the query API, `files` by the storage
        // metadata endpoint; everything else is a generated CRUD view.
        case "organisations":
            return "/api/v1/query/organisations";
        case "files":
            return "/api/v1/storage/files/metadata";
        default:
            return `/api/v1/${t_table}`;
    }
}

export function useQueryConsole() {
    const { showEvent } = useEvent();
    const form = useFormState({ table: "automated-services", params: "" });
    // Whatever the server returned — a row array, or an error document. Shown as pretty JSON.
    const [queryResult, setQueryResult] = useState<unknown>(null);
    const running = usePending();

    const executeQuery = () =>
        running.run(async () => {
            setQueryResult(null);
            try {
                const { table, params } = form.values;
                const endpoint = endpointFor(table);
                const url = params ? `${endpoint}?${params.startsWith("?") ? params.slice(1) : params}` : endpoint;

                const response = await authenticatedFetch(url);
                const data = await readBody<{ message?: string }>(response);
                setQueryResult(data);
                if (!response.ok) {
                    showEvent("error", `Query failed: ${data.message || response.statusText}`);
                } else {
                    showEvent("success", "Query executed successfully");
                }
            } catch (error) {
                console.error("Query failed:", error);
                showEvent("error", errorMessage(error, "Failed to execute query."));
            }
        });

    return {
        queryTable: form.values.table,
        setQueryTable: (t_table: string) => form.setField("table", t_table),
        queryParams: form.values.params,
        setQueryParams: (t_params: string) => form.setField("params", t_params),
        queryResult,
        queryLoading: running.pending,
        executeQuery,
    };
}
