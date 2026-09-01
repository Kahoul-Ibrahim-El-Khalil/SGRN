export const AdminBackendApiEndpoints = {
    REGISTER_USER: "/api/v1/admin/users/register",
    LIST_USERS: "/api/v1/admin/users",
    REGISTER_AUTOMATED_SERVICE: "/api/v1/admin/automated-services/register",
    LIST_AUTOMATED_SERVICES: "/api/v1/admin/automated-services",
    UPDATE_AUTOMATED_SERVICE_METADATA: (id: number) => `/api/v1/admin/automated-services/${id}/metadata`,
    ROTATE_AUTOMATED_SERVICE_TOKEN: "/api/v1/admin/automated-services/rotate-token",
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
    LIST_DOMAINS: "/api/v1/query/domains",
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

    // File Listing Operations
    FILES_BY_USER: "/api/v1/storage/files/byUser",
    FILES_BY_DOMAIN: "/api/v1/storage/files/byDomain",
    FILES_BY_PATH: "/api/v1/storage/files/byPath",
    FILES_BY_EXTENSION: "/api/v1/storage/files/byExtension",
    FILES_BY_SUBMISSION: "/api/v1/storage/files/bySubmission",
    FILES_SEARCH: "/api/v1/storage/files/search",

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
