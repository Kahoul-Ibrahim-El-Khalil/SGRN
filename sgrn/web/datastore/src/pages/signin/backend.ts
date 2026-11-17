// @/pages/signin/backend.ts
import { QueryListBackendApiEndpoints, SessionBackendApiEndpoints } from "@/backend/endpoints";
import { handleApiError } from "@/backend/errorHandler";
import { authenticatedFetch, jsonBody, processResponse, readBody } from "@/backend/api/fetcher";
import type { SignInRequest } from "@sgrn/datastore/requests";
import type { IdNamePair } from "@/types/IdNamePair";
import type { LoginResponse } from "./types.ts";
import type { SgrnResult } from "@sgrn/types";
import { isSuccess } from "@sgrn/types";

export async function fetchOrganisations(): Promise<IdNamePair[]> {
    try {
        const res = await authenticatedFetch(QueryListBackendApiEndpoints.LIST_ORGANISATIONS, {});
        const data = await readBody<IdNamePair[]>(res);
        if (Array.isArray(data)) return data;
        console.error("Unexpected organisations response", data);
        return [];
    } catch (err) {
        handleApiError("Network error: Could not fetch organisations");
        return [];
    }
}

export async function fetchDomains(): Promise<IdNamePair[]> {
    try {
        // Tenant-scoped server-side: the session's organisation determines
        // the rows, so no organisation parameter is sent (or trusted).
        const res = await authenticatedFetch(QueryListBackendApiEndpoints.LIST_DOMAINS, {});
        const data = await readBody<IdNamePair[]>(res);
        if (Array.isArray(data)) return data;
        return [];
    } catch (err) {
        handleApiError("Network error: Could not fetch domains");
        return [];
    }
}

export async function fetchStatuses(t_organisation: string): Promise<IdNamePair[]> {
    try {
        const url = `${QueryListBackendApiEndpoints.LIST_STATUSES}?organisation=${encodeURIComponent(t_organisation)}`;
        const res = await authenticatedFetch(url, {});
        const data = await readBody<IdNamePair[]>(res);
        if (Array.isArray(data)) return data;
        return [];
    } catch (err) {
        handleApiError("Network error: Could not fetch statuses");
        return [];
    }
}

export interface SignInPayload {
    email: string;
    password: string;
}

export async function doSignIn(t_signin_payload: SignInPayload): Promise<SgrnResult<LoginResponse>> {
    const encoded = jsonBody<SignInRequest>(t_signin_payload);
    // authenticatedFetch also sets Accept: application/json, so the server
    // answers with JSON rather than its binary default.
    const res = await authenticatedFetch(SessionBackendApiEndpoints.SIGN_IN, {
        method: "POST",
        headers: encoded.headers,
        body: encoded.body,
    });

    const result = await processResponse<LoginResponse>(res);

    if (isSuccess(result)) {
        const data = result.data;
        // Cache token and user on success
        if (data.token) {
            sessionStorage.setItem("SGRN-TOKEN", data.token);
            localStorage.setItem("SGRN-TOKEN", data.token);
        }
        if (data.user) {
            const userStr = JSON.stringify(data.user);
            sessionStorage.setItem("user_info", userStr);
            localStorage.setItem("user_info", userStr);
        }
    }

    return result;
}

export async function handleSignOut() {
    try {
        await authenticatedFetch(SessionBackendApiEndpoints.SIGN_OUT, {
            method: "POST",
        });
    } finally {
        localStorage.removeItem("SGRN-TOKEN");
        localStorage.removeItem("user_info");
        sessionStorage.removeItem("SGRN-TOKEN");
        sessionStorage.removeItem("user_info");
    }
}
