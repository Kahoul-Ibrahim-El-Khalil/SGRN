// ==========================================
// Administration, Governance & Audit
// ==========================================

export interface OrganisationRow {
  name: string;
  description?: string | null;
  domain?: string | null;
  status: string;
  total_virtual_size: number;
  storage_limit?: number | null;
  total_entry_count: number;
  entry_count_limit?: number | null;
  rate_limit_rpm?: number | null;
}

export interface OrganisationListResponse {
  organisations: OrganisationRow[];
}

export interface OrganisationCreatedResponse {
  organisation: string;
  admin_user_id?: number;
}

export interface OrganisationUpdatedResponse {
  organisation: string;
}

export interface RoleCreatedResponse {
  role_id: number;
  message: string;
}

export interface RoleAssignmentEntry {
  role_id: number;
  role_name: string;
  target_type: string;
  target_id: number;
  target_label: string;
}

export interface RoleAssignmentsResponse {
  assignments: RoleAssignmentEntry[];
  count: number;
}

export interface RoleListEntry {
  id: number;
  name: string;
  description: string;
  is_system: boolean;
  created_at: string;
  updated_at: string;
  permissions: any[];
}

export interface RoleListResponse {
  roles: RoleListEntry[];
}

export interface PermissionRow {
  id: number;
  user_id: number;
  email: string;
  organisation: string;
  domain: string;
  allowed_subpath: string;
  can_read: boolean;
  can_write: boolean;
  can_delete: boolean;
}

export interface PermissionListResponse {
  permissions: PermissionRow[];
  count: number;
}

export interface PermissionGrantResponse extends PermissionRow {
  note?: string | null;
}

export interface PermissionRevokeResponse {
  revoked: string;
  warning?: string | null;
}

export interface AuditLogEntry {
  id: number;
  organisation: string;
  actor_type: string;
  actor_id?: number | null;
  actor_name: string;
  action: string;
  target_type: string;
  target_id: string;
  ip: string;
  status: string;
  created_at: string;
  details: Record<string, any>;
}

export interface AuditLogsResponse {
  audit_logs: AuditLogEntry[];
  limit: number;
  offset: number;
}

export interface AuditVerificationResponse {
  is_valid: boolean;
  total_rows: number;
  verified_checkpoints: number;
  status: string;
  message: string;
  first_broken_seq?: number | null;
  first_broken_id?: number | null;
}

export interface AuditPurgeResponse {
  records_purged: number;
  older_than_days: number;
  message: string;
}

export interface ServerStatusResponse {
  status: string;
  version: string;
  timestamp: string;
}

export interface EndpointsResponse {
  endpoints: string[];
}

export interface SystemConfigResponse {
  config_path: string;
  values: Record<string, any>;
  schema: any[];
  live: Record<string, any>;
  pending_restart: string[];
}

export interface SystemConfigUpdateResponse {
  config_path: string;
  backup_path: string;
  applied_hot: string[];
  pending_restart: string[];
  warnings: string[];
}
