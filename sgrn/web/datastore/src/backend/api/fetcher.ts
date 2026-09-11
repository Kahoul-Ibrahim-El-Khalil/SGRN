// @/backend/api/fetcher.ts

import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export const SGRN_TOKEN_KEY_IDENTIFIER: string = "Authorization";

export async function authenticatedFetch(t_url: string, t_http_options: RequestInit = {}) {
    // Get token
    let token = sessionStorage.getItem("SGRN-TOKEN");

    // Debug logging
    console.log(`[Auth Fetch] URL: ${t_url}, Token present: ${!!token}`);

    const headers = new Headers(t_http_options.headers || {});

    // Add token to headers using standard Authorization: Bearer <token>
    if (token) {
        headers.set("Authorization", `Bearer ${token}`);
    }

    const response = await fetch(t_url, {
        ...t_http_options,
        headers: headers,
        credentials: "include", // Always include for better CORS compatibility
    });

    // Handle auth errors
    if (response.status === 401) {
        console.warn(`[Auth Fetch] 401 Unauthorized at ${t_url}`);
        sessionStorage.removeItem("SGRN-TOKEN");

        // Only redirect if not already on signin page
        if (!window.location.pathname.includes("/signin")) {
            window.location.href = "/signin";
        }
        return response;
    }

    return response;
}

/**
 * Helper to process SgrnResult from a Fetch Response
 */
export async function processResponse<T>(t_response: Response): Promise<SgrnResult<T>> {
    const contentType = t_response.headers.get("Content-Type") || "";
    const isJson = contentType.includes("application/json");

    // Handle Gateway/Network level errors early
    if (t_response.status === 502 || t_response.status === 503 || t_response.status === 504) {
        return {
            error: `Server is currently unavailable (${t_response.status}: ${t_response.statusText})`,
            scope: ErrorScope.Network,
        };
    }

    try {
        if (!isJson) {
            const text = await t_response.text();
            return {
                error: t_response.ok ? "Expected JSON but received text" : text.slice(0, 100) || `HTTP Error ${t_response.status}`,
                scope: t_response.ok ? ErrorScope.Runtime : ErrorScope.Unknown,
            };
        }

        const data = await t_response.json();

        if (t_response.ok) {
            return { data: data as T };
        } else {
            return {
                error: data.error || `HTTP Error ${t_response.status}`,
                scope: data.scope || ErrorScope.Unknown,
            };
        }
    } catch (e) {
        return {
            error: `Communication Error: ${e instanceof Error ? e.message : String(e)}`,
            scope: ErrorScope.Network,
        };
    }
}
