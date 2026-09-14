export const AdminBackendApiEndpoints = {
    REGISTER_USER: "/api/v1/admin/users/register",
    LIST_USERS: "/api/v1/admin/users",
    REGISTER_AUTOMATED_SERVICE: "/api/v1/admin/automated-services/register",
    LIST_AUTOMATED_SERVICES: "/api/v1/automated-services",
    UPDATE_AUTOMATED_SERVICE_METADATA: (id: number) => `/api/v1/admin/automated-services/${id}/metadata`,
    ROTATE_AUTOMATED_SERVICE_TOKEN: "/api/v1/admin/automated-services/rotate-token",
    STORAGE_OVERVIEW: "/api/v1/admin/storage/overview",
    STORAGE_ORPHANS: "/api/v1/admin/storage/orphans",
    STORAGE_PURGE_ORPHANS: "/api/v1/admin/storage/orphans/purge",
    STORAGE_SEARCH: "/api/v1/admin/storage/search",
    SYSTEM_CONFIG: "/api/v1/admin/system/config",
    STORAGE_FORMATS: "/api/v1/admin/storage/formats",
    QUOTAS: "/api/v1/admin/quotas",
    PERMISSIONS: "/api/v1/admin/permissions",
    ANALYTICS_OVERVIEW: "/api/v1/admin/analytics/overview",
    ANALYTICS_BREAKDOWN: "/api/v1/admin/analytics/breakdown",
} as const;
export const SessionBackendApiEndpoints = {
    SIGN_IN: "/api/v1/auth/user/signin",
    SIGN_OUT: "/api/v1/auth/user/signout",
    UPDATE_PASSWORD: "/api/v1/auth/user/password",
} as const;

export type SessionBackendApiEndpoint = (typeof SessionBackendApiEndpoints)[keyof typeof SessionBackendApiEndpoints];

export const QueryListBackendApiEndpoints = {
    LIST_ORGANISATIONS: "/api/v1/query/organisations",
    LIST_STATUSES: "/api/v1/query/statuses",
    LIST_DOMAINS: "/api/v1/domains",
    QUERY_USER_INFO: "/api/v1/query/user/info",
    UPDATE_USER_INFO: "/api/v1/query/user/info",
} as const;

export type QueryListBackendApiEndpoint = (typeof QueryListBackendApiEndpoints)[keyof typeof QueryListBackendApiEndpoints];

// Storage API Endpoints (Consolidated)
export const StorageBackendApiEndpoints = {
    // Object Operations
    UPLOAD: "/api/v1/storage/files",
    DOWNLOAD: "/api/v1/storage/files",
    DELETE: "/api/v1/storage/drive/delete",

    // Configuration Operations
    CONSTRAINTS: "/api/v1/storage/info",
    GET_STATS: "/api/v1/storage/stats",

    // File Listing Operations (in-process metadata endpoint;
    // supports ?col=op.value filters, ?order=, ?limit=, ?offset=)
    FILES_METADATA: "/api/v1/storage/files/metadata",

    // Drive Operations
    DRIVE_LIST: "/api/v1/storage/drive/list",
    DRIVE_MKDIR: "/api/v1/storage/drive/mkdir",
    DRIVE_MOVE: "/api/v1/storage/drive/move",
    DRIVE_DELETE: "/api/v1/storage/drive/delete",
    DRIVE_ZIP: "/api/v1/storage/drive/zip",
    DRIVE_BULK: "/api/v1/storage/drive/bulk",
    PATH_BASE: "/api/v1/storage/files",
} as const;

export type StorageBackendApiEndpoint = (typeof StorageBackendApiEndpoints)[keyof typeof StorageBackendApiEndpoints];
