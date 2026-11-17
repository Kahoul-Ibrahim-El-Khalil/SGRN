// @/backend/api/fetcher.ts

import { appPath } from "@/lib/appPath";
import { authenticatedFetch as sharedAuthenticatedFetch, jsonBody, processResponse, rawJson } from "@sgrn/datastore/web";

export async function authenticatedFetch(t_url: string, t_http_options: RequestInit = {}) {
    return sharedAuthenticatedFetch(t_url, t_http_options, {
        onUnauthorized: () => {
            if (!window.location.pathname.includes("/signin")) {
                window.location.href = appPath("/signin");
            }
        },
    });
}

/**
 * Read the JSON body of a Response. Drop-in replacement for `res.json()`.
 * Falls back to `{}` on parse failure (matching `.catch(() => ({}))` patterns).
 */
export async function readBody<T = any>(t_res: Response, t_fallback?: any): Promise<T> {
    try {
        return (await t_res.json()) as T;
    } catch {
        return (t_fallback !== undefined ? t_fallback : {}) as T;
    }
}

/**
 * Await a JSON response and unwrap an SgrnResult, throwing the server's
 * message on failure. This is the single place browser callers turn a
 * non-2xx response into an exception, so route wrappers stay declarative.
 */
export async function expectOk<T = unknown>(t_res: Response, t_message = "Request failed"): Promise<T> {
    const result = await processResponse<T>(t_res);
    if (result.error !== undefined) {
        throw new Error(result.error || t_message);
    }
    return result.data as T;
}

/**
 * Helper to process SgrnResult from a Fetch Response
 */
export { jsonBody, processResponse, rawJson };
