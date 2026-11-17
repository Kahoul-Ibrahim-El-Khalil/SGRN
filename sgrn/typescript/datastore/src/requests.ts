// Request DTOs for the typed endpoints of sgrn_datastore.
//
// Each interface mirrors the C++ request struct that the handler reads with
// `parseTypedRequest<T>()`; the C++ name is given in the comment. Fields that
// are `std::optional<T>` or carry a C++ default are optional here.
// `std::optional<std::optional<T>>` ("absent" vs "set to null") is typed
// `T | null` — note that glaze reads a nil into the outer optional, so on the
// wire both null and absent mean "not provided".

import type { RawJsonString } from "./wire";

/** Any JSON value, as accepted by the server's open-ended document fields. */
export type JsonValue =
  string | number | boolean | null | JsonObject | JsonValue[];

/** A JSON object with arbitrary keys (stored `jsonb` documents). */
export interface JsonObject {
  [key: string]: JsonValue;
}

/** SignInRequest */
export interface SignInRequest {
  email: string;
  password: string;
}

/** ServiceSignInRequest */
export interface ServiceSignInRequest {
  token: string;
  secret: string;
}

/** UpdatePasswordRequest */
export interface UpdatePasswordRequest {
  old_password: string;
  new_password: string;
}

/** UpdateUserInfoRequest */
export interface UpdateUserInfoRequest {
  first_name: string;
  family_name: string;
  phone_number: string;
}

/** RegisterUserRequest */
export interface RegisterUserRequest {
  first_name: string;
  family_name: string;
  email: string;
  password: string;
  phone_number?: string;
  organisation: string;
  status?: string;
  domain?: string;
  storage_limit?: number | null;
  role?: string;
  is_platform_admin?: boolean;
}

/** RegisterAutomatedServiceRequest (`organisation` is also read by the scope filter's OrgBodyProbe) */
export interface RegisterAutomatedServiceRequest {
  name: string;
  kind?: string;
  metadata?: RawJsonString;
  storage_limit?: number | null;
  domain?: string | null;
  organisation?: string;
}

/** UpdateAutomatedServiceMetadataRequest */
export interface UpdateAutomatedServiceMetadataRequest {
  metadata?: RawJsonString;
  description?: string;
}

/** RotateAutomatedServiceTokenRequest */
export interface RotateAutomatedServiceTokenRequest {
  automated_service_id: number;
}

/** SetStatusRequest */
export interface SetStatusRequest {
  status: string;
}

/** ResetPasswordRequest */
export interface ResetPasswordRequest {
  new_password: string;
}

/** RegisterWebhookRequest */
export interface RegisterWebhookRequest {
  url: string;
  secret: string;
  events?: string[];
}

/** RenameDomainRequest */
export interface RenameDomainRequest {
  name: string;
}

/** MoveDomainRequest */
export interface MoveDomainRequest {
  parent_id?: number | null;
}

/** CreateOrganisationAdminPayload */
export interface CreateOrganisationAdminPayload {
  email: string;
  first_name: string;
  family_name: string;
  password: string;
}

/** CreateOrganisationRequest */
export interface CreateOrganisationRequest {
  name: string;
  description?: string | null;
  domain?: string | null;
  storage_limit?: number | null;
  entry_count_limit?: number | null;
  admin?: CreateOrganisationAdminPayload;
}

/** UpdateOrganisationRequest */
export interface UpdateOrganisationRequest {
  description?: string | null;
  domain?: string | null;
  status?: string | null;
  storage_limit?: number | null;
  entry_count_limit?: number | null;
  rate_limit_rpm?: number | null;
}

/** GrantPermissionRequest */
export interface GrantPermissionRequest {
  user_id?: number | null;
  email?: string | null;
  domain: string;
  organisation?: string | null;
  allowed_subpath?: string;
  can_read?: boolean;
  can_write?: boolean;
  can_delete?: boolean;
}

/** QuotaUpdateRequest */
export interface QuotaUpdateRequest {
  storage_limit?: number | null;
  entry_count_limit?: number | null;
  rate_limit_rpm?: number | null;
  max_file_size_mb?: number | null;
  preferred_chunk_size_mb?: number | null;
  rate_limit_upload_rpm?: number | null;
}

/** CreateRoleRequest (`permissions` is a RawJson array of permission names) */
export interface CreateRoleRequest {
  name: string;
  description?: string;
  permissions?: RawJsonString<string[]>;
}

/** UpdateRoleRequest */
export interface UpdateRoleRequest {
  description?: string;
  permissions?: RawJsonString<string[]>;
}

/** RoleTargetRequest */
export interface RoleTargetRequest {
  role_id: number;
  user_id?: number | null;
  automated_service_id?: number | null;
}

/** UpdateSystemConfigRequest (`values` is a RawJson object) */
export interface UpdateSystemConfigRequest {
  values: RawJsonString<Record<string, unknown>>;
}

/** UpsertFormatRequest */
export interface UpsertFormatRequest {
  extension: string;
  mime_type: string;
  is_compressed?: boolean;
  is_allowed?: boolean;
  description?: string | null;
}

/** PurgeOrphansRequest */
export interface PurgeOrphansRequest {
  bucket: string;
  dry_run?: boolean;
  limit?: number;
  keys?: string[];
  prefix?: string | null;
}

/** MoveItemRequest */
export interface MoveItemRequest {
  name?: string;
  new_name?: string;
  parent_id?: number | null;
}

/** BulkItem */
export interface BulkItem {
  id: number | string;
  type: string;
}

/** BulkActionRequest */
export interface BulkActionRequest {
  action: string;
  items: BulkItem[];
  target_parent_id?: number | null;
}

/** UploadInitRequest */
export interface UploadInitRequest {
  filename: string;
  target_path?: string;
  scope: string;
  mime_type?: string;
  total_size: number;
  chunk_size: number;
  encrypt?: boolean;
}

/** UploadCompleteRequest */
export interface UploadCompleteRequest {
  upload_id: string;
}
