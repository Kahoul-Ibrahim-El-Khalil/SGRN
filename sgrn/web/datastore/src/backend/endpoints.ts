// Compatibility facade for older dashboard imports.
// The canonical API boundary lives in @sgrn/datastore.
export {
    AdminBackendApiEndpoints,
    QueryListBackendApiEndpoints,
    SessionBackendApiEndpoints,
    StorageBackendApiEndpoints,
} from "@sgrn/datastore/web";

export type { BrowserApiOptions } from "@sgrn/datastore/web";

import {
    AdminBackendApiEndpoints,
    QueryListBackendApiEndpoints,
    SessionBackendApiEndpoints,
    StorageBackendApiEndpoints,
} from "@sgrn/datastore/web";

export type AdminBackendApiEndpoint = (typeof AdminBackendApiEndpoints)[keyof typeof AdminBackendApiEndpoints];
export type QueryListBackendApiEndpoint = (typeof QueryListBackendApiEndpoints)[keyof typeof QueryListBackendApiEndpoints];
export type SessionBackendApiEndpoint = (typeof SessionBackendApiEndpoints)[keyof typeof SessionBackendApiEndpoints];
export type StorageBackendApiEndpoint = (typeof StorageBackendApiEndpoints)[keyof typeof StorageBackendApiEndpoints];
