// @/backend/api/domains.ts
//
// Typed wrappers for the domain routes. Every function resolves to the decoded
// result and throws an Error carrying the server's message on failure, so
// callers (and the domains store) handle errors in one place.
import { AdminBackendApiEndpoints, QueryListBackendApiEndpoints } from "@/backend/endpoints";
import { authenticatedFetch, expectOk, jsonBody } from "@/backend/api/fetcher";
import type { DomainsCreateRequest, DomainsUpdateRequest } from "@sgrn/datastore/crud-requests";
import type { MoveDomainRequest, RenameDomainRequest } from "@sgrn/datastore/requests";
import type { DomainRecord } from "@/types/records";

const DOMAIN_ITEM = (t_id: number) => `${QueryListBackendApiEndpoints.LIST_DOMAINS}/${t_id}`;

/** GET /api/v1/domains — tenant-scoped server-side by the session's organisation. */
export async function listDomains(t_signal?: AbortSignal): Promise<DomainRecord[]> {
    const res = await authenticatedFetch(QueryListBackendApiEndpoints.LIST_DOMAINS, {
        signal: t_signal,
    });
    const data = await expectOk<DomainRecord[]>(res, "Failed to load domains");
    if (!Array.isArray(data)) {
        throw new Error("Unexpected domain list format");
    }
    return data;
}

export async function createDomain(t_body: DomainsCreateRequest): Promise<void> {
    const res = await authenticatedFetch(QueryListBackendApiEndpoints.LIST_DOMAINS, {
        method: "POST",
        ...jsonBody<DomainsCreateRequest>(t_body),
    });
    await expectOk(res, "Failed to create domain");
}

/** PATCH of the generated CRUD view: only the encryption policy is updatable. */
export async function updateDomain(t_id: number, t_body: DomainsUpdateRequest): Promise<void> {
    const res = await authenticatedFetch(DOMAIN_ITEM(t_id), {
        method: "PATCH",
        ...jsonBody<DomainsUpdateRequest>(t_body),
    });
    await expectOk(res, "Failed to update domain");
}

/** Lifecycle route: re-labels every authority in the sub-tree. */
export async function renameDomain(t_id: number, t_body: RenameDomainRequest): Promise<void> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.DOMAIN_RENAME(t_id), {
        method: "POST",
        ...jsonBody<RenameDomainRequest>(t_body),
    });
    await expectOk(res, "Failed to rename domain");
}

/** Lifecycle route: re-parent a sub-tree (`parent_id: null` moves it to the root). */
export async function moveDomain(t_id: number, t_body: MoveDomainRequest): Promise<void> {
    const res = await authenticatedFetch(AdminBackendApiEndpoints.DOMAIN_MOVE(t_id), {
        method: "POST",
        ...jsonBody<MoveDomainRequest>(t_body),
    });
    await expectOk(res, "Failed to move domain");
}

export async function deleteDomain(t_id: number): Promise<void> {
    const res = await authenticatedFetch(DOMAIN_ITEM(t_id), { method: "DELETE" });
    await expectOk(res, "Failed to delete domain");
}
