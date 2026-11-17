// ==========================================
// Common Types, Errors & System Enums
// ==========================================

export const ErrorScope = {
  Runtime: "Runtime",
  Database: "Database",
  Garage: "Garage",
  Compression: "Compression",
  Hashing: "Hashing",
  ApplicationLogic: "ApplicationLogic",
  Authentication: "Authentication",
  Authorization: "Authorization",
  FileSystem: "FileSystem",
  Network: "Network",
  Filter: "Filter",
  Service: "Service",
  Allocation: "Allocation",
  PLC: "PLC",
  // API-domain scopes (from ApiErrors code_name prefix)
  AdminApi: "AdminApi",
  AuthApi: "AuthApi",
  QueryApi: "QueryApi",
  Sgrn: "Sgrn",
  Unknown: "Unknown",
} as const;

export type ErrorScope = (typeof ErrorScope)[keyof typeof ErrorScope];

export interface SgrnError {
  error: string;
  scope: ErrorScope;
}

export type SgrnResult<T> =
  | {
      data: T;
      error?: never;
      scope?: never;
    }
  | {
      error: string;
      scope: ErrorScope;
      data?: never;
    };

/**
 * Type guard to check if a result is successful
 */
export function isSuccess<T>(result: SgrnResult<T>): result is {
  data: T;
  error?: never;
} {
  return result.error === undefined;
}

/**
 * Type guard to check if a result is an error
 */
export function isError<T>(result: SgrnResult<T>): result is {
  error: string;
  scope: ErrorScope;
  data?: never;
} {
  return result.error !== undefined;
}

export interface ApiError {
  status: number;
  message: string;
}

export interface ApiResponse<T = any> {
  success: boolean;
  result?: string;
  data?: T;
  error?: string;
}

export interface Credentials {
  token: string;
  secret: string;
}

export interface MessageResponse {
  message: string;
}

export interface IdNameResponse {
  id: number | string;
  name: string;
}

export interface SuccessMessageResponse {
  message: string;
}
