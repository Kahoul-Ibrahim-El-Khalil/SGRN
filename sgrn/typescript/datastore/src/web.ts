import type { SgrnResult } from "@sgrn/types";
import { ErrorScope } from "@sgrn/types";

export interface BrowserApiOptions {
  tokenStorageKey?: string;
  credentials?: RequestCredentials;
  onUnauthorized?: (response: Response) => void;
}

const defaultOptions: Required<Omit<BrowserApiOptions, "onUnauthorized">> = {
  tokenStorageKey: "SGRN-TOKEN",
  credentials: "include",
};

export async function authenticatedFetch(
  t_url: string,
  t_http_options: RequestInit = {},
  t_options: BrowserApiOptions = {},
): Promise<Response> {
  const options = { ...defaultOptions, ...t_options };
  const token = sessionStorage.getItem(options.tokenStorageKey);
  const headers = new Headers(t_http_options.headers || {});

  if (token) {
    headers.set("Authorization", `Bearer ${token}`);
  }

  const response = await fetch(t_url, {
    ...t_http_options,
    headers,
    credentials: options.credentials,
  });

  if (response.status === 401) {
    sessionStorage.removeItem(options.tokenStorageKey);
    options.onUnauthorized?.(response);
  }

  return response;
}

export async function processResponse<T>(
  t_response: Response,
): Promise<SgrnResult<T>> {
  const contentType = t_response.headers.get("Content-Type") || "";

  if ([502, 503, 504].includes(t_response.status)) {
    return {
      error: `Server is currently unavailable (${t_response.status}: ${t_response.statusText})`,
      scope: ErrorScope.Network,
    };
  }

  try {
    if (!contentType.includes("application/json")) {
      const text = await t_response.text();
      return {
        error: t_response.ok
          ? "Expected JSON but received text"
          : text.slice(0, 100) || `HTTP Error ${t_response.status}`,
        scope: t_response.ok ? ErrorScope.Runtime : ErrorScope.Unknown,
      };
    }

    const data = (await t_response.json()) as {
      error?: string;
      scope?: ErrorScope;
    } & T;
    if (t_response.ok) return { data: data as T };

    return {
      error: data.error || `HTTP Error ${t_response.status}`,
      scope: data.scope || ErrorScope.Unknown,
    };
  } catch (t_error) {
    return {
      error: `Communication Error: ${t_error instanceof Error ? t_error.message : String(t_error)}`,
      scope: ErrorScope.Network,
    };
  }
}

export const AdminBackendApiEndpoints = {
  REGISTER_USER: "/api/v1/admin/users/register",
  LIST_USERS: "/api/v1/admin/users",
  REGISTER_AUTOMATED_SERVICE: "/api/v1/admin/automated-services/register",
  LIST_AUTOMATED_SERVICES: "/api/v1/automated-services",
  UPDATE_AUTOMATED_SERVICE_METADATA: (id: number) =>
    `/api/v1/admin/automated-services/${id}/metadata`,
  AUTOMATED_SERVICE_ITEM: (id: number) =>
    `/api/v1/admin/automated-services/${id}`,
  AUTOMATED_SERVICE_STATUS: (id: number) =>
    `/api/v1/admin/automated-services/${id}/status`,
  ROTATE_AUTOMATED_SERVICE_TOKEN:
    "/api/v1/admin/automated-services/rotate-token",
  STORAGE_OVERVIEW: "/api/v1/admin/storage/overview",
  STORAGE_ORPHANS: "/api/v1/admin/storage/orphans",
  STORAGE_PURGE_ORPHANS: "/api/v1/admin/storage/orphans/purge",
  STORAGE_SEARCH: "/api/v1/admin/storage/search",
  SYSTEM_CONFIG: "/api/v1/admin/system/config",
  STORAGE_FORMATS: "/api/v1/admin/storage/formats",
  QUOTAS: "/api/v1/admin/quotas",
  ROLES: "/api/v1/admin/roles",
  ROLES_ITEM: (id: number) => `/api/v1/admin/roles/${id}`,
  ROLES_ASSIGN: "/api/v1/admin/roles/assign",
  ROLES_REVOKE: "/api/v1/admin/roles/revoke",
  AUDIT_LOGS: "/api/v1/admin/audit-logs",
  AUDIT_VERIFY: "/api/v1/admin/audit-logs/verify",
  ROLE_ASSIGNMENTS: "/api/v1/admin/roles/assignments",
  QUOTAS_DOMAIN: (id: number) => `/api/v1/admin/quotas/domain/${id}`,
  DOMAINS: "/api/v1/admin/domains",
  DOMAIN_ITEM: (id: number) => `/api/v1/admin/domains/${id}`,
  DOMAIN_RENAME: (id: number) => `/api/v1/admin/domains/${id}/rename`,
  DOMAIN_MOVE: (id: number) => `/api/v1/admin/domains/${id}/move`,
  ACCOUNT_ADMIN: "/api/v1/admin/users/register",
  ACCOUNTS: "/api/v1/admin/users",
  ACCOUNT_ITEM: (id: number) => `/api/v1/admin/users/${id}`,
  ACCOUNT_STATUS: (id: number) => `/api/v1/admin/users/${id}/status`,
  ACCOUNT_PASSWORD: (id: number) => `/api/v1/admin/users/${id}/password`,
  QUOTAS_ORG: "/api/v1/admin/quotas/organisation",
  ORGANISATIONS: "/api/v1/admin/organisations",
  QUOTAS_USER: (id: number) => `/api/v1/admin/quotas/user/${id}`,
  QUOTAS_SERVICE: (id: number) => `/api/v1/admin/quotas/service/${id}`,
  PERMISSIONS: "/api/v1/admin/permissions",
  ANALYTICS_OVERVIEW: "/api/v1/admin/analytics/overview",
  ANALYTICS_BREAKDOWN: "/api/v1/admin/analytics/breakdown",
  METAPROBE_SESSIONS: "/api/v1/admin/metaprobe/sessions",
  METAPROBE_SESSION_ITEM: (token: string) =>
    `/api/v1/admin/metaprobe/sessions/${encodeURIComponent(token)}`,
  WEBHOOKS: "/api/v1/admin/webhooks",
  WEBHOOK_ITEM: (id: number) => `/api/v1/admin/webhooks/${id}`,
} as const;

export const SessionBackendApiEndpoints = {
  SIGN_IN: "/api/v1/auth/user/signin",
  SIGN_OUT: "/api/v1/auth/user/signout",
  UPDATE_PASSWORD: "/api/v1/auth/user/password",
} as const;

export const QueryListBackendApiEndpoints = {
  LIST_ORGANISATIONS: "/api/v1/query/organisations",
  LIST_STATUSES: "/api/v1/query/statuses",
  LIST_DOMAINS: "/api/v1/domains",
  QUERY_USER_INFO: "/api/v1/query/user/info",
  UPDATE_USER_INFO: "/api/v1/query/user/info",
} as const;

export const StorageBackendApiEndpoints = {
  UPLOAD: "/api/v1/storage/files",
  DOWNLOAD: "/api/v1/storage/files",
  DELETE: "/api/v1/storage/drive/delete",
  UPLOAD_INIT: "/api/v1/storage/upload/init",
  UPLOAD_CHUNK: "/api/v1/storage/upload/chunk",
  UPLOAD_STATUS: "/api/v1/storage/upload/status",
  UPLOAD_COMPLETE: "/api/v1/storage/upload/complete",
  UPLOAD_ABORT: "/api/v1/storage/upload/abort",
  CONSTRAINTS: "/api/v1/storage/info",
  GET_STATS: "/api/v1/storage/stats",
  FILES_METADATA: "/api/v1/storage/files/metadata",
  DRIVE_LIST: "/api/v1/storage/drive/list",
  DRIVE_MKDIR: "/api/v1/storage/drive/mkdir",
  DRIVE_MOVE: "/api/v1/storage/drive/move",
  DRIVE_DELETE: "/api/v1/storage/drive/delete",
  DRIVE_ZIP: "/api/v1/storage/drive/zip",
  DRIVE_BULK: "/api/v1/storage/drive/bulk",
  PATH_BASE: "/api/v1/storage/files",
} as const;
