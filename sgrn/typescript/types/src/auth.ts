// ==========================================
// Authentication, Users & Service Sessions
// ==========================================

export interface Role {
  name: string;
  code?: number;
}

export interface Domain {
  id: number;
  name: string;
}

export interface Organisation {
  id: number;
  name: string;
}

export interface User {
  id: number;
  user_id?: number; // alias
  first_name: string;
  family_name: string;
  email: string;
  phone_number?: string | null;
  role?: string | Role;
  domain?: Domain | string;
  organisation?: Organisation | string;
  created_at?: string;
  is_active?: boolean;
  status?: string;
}

export interface AutomatedServiceSession {
  automated_service_id: number;
  automated_service?: Record<string, unknown>;
  session_id?: string;
}

export interface UserListEntry {
  id: number;
  email: string;
  first_name: string;
  family_name: string;
  organisation: string;
  domain?: string | null;
  phone_number?: string | null;
  status?: string;
  can_read_personal?: boolean;
  can_write_personal?: boolean;
  can_delete_personal?: boolean;
}

export interface ServiceRegistrationResponse {
  message?: string;
  error?: string;
  automated_service_id: number;
  name: string;
  token: string;
  token_secret: string;
  organisation: string;
  kind: string;
}

export interface ServiceRotationResponse {
  error?: string;
  automated_service_id: number;
  name: string;
  token: string;
  token_secret: string;
  is_active: boolean;
  status: string;
  organisation: string;
  created_at: string;
  metadata: Record<string, any>;
  kind?: any;
}

export interface UserStatusResponse {
  status: string;
}

export interface DeletedUserResponse {
  deleted_user_id: number;
}

export interface ServiceStatusResponse {
  automated_service_id: number;
  status: string;
}

export interface DeletedServiceResponse {
  deleted_service_id: number;
}

export interface UserInfoResponse {
  id: number;
  email: string;
  first_name: string;
  family_name: string;
  organisation: string;
  role: string;
  phone_number?: string | null;
  created_at: string;
  last_login?: string | null;
}

export interface ActiveSessionResponse {
  token: string;
  user_id?: number | null;
  automated_service_id?: number | null;
  ip: string;
  created_at: string;
  expires_at: string;
  actor_type: string;
  actor_name: string;
}

export interface SessionRevokedResponse {
  revoked_session: string;
}
