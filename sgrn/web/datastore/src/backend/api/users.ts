// @/backend/api/users.ts — typed wrapper for the generated users CRUD view.
import { authenticatedFetch, expectOk } from "@/backend/api/fetcher";
import type { UserRecord } from "@/types/records";

/** GET /api/v1/users — resolves to the roster, throws the server message on failure. */
export async function listUsers(t_signal?: AbortSignal): Promise<UserRecord[]> {
    const res = await authenticatedFetch("/api/v1/users", { signal: t_signal });
    const data = await expectOk<UserRecord[]>(res, "Failed to load user roster");
    if (!Array.isArray(data)) {
        throw new Error("Unexpected user list format");
    }
    return data;
}
