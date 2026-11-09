// @/backend/api/fetcher.ts

import { appPath } from "@/lib/appPath";
import { authenticatedFetch as sharedAuthenticatedFetch, processResponse } from "@sgrn/datastore/web";

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
 * Helper to process SgrnResult from a Fetch Response
 */
export { processResponse };
