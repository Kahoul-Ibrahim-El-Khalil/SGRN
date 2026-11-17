// ==========================================
// Storage Analytics, Quotas & Breakdown Metrics
// ==========================================

export interface AnalyticsCounts {
  users: number;
  automated_services: number;
  organisations: number;
  domains: number;
  files: number;
  objects: number;
  formats: number;
  permission_grants: number;
}

export interface AnalyticsBytes {
  virtual_bytes: number;
  stored: number;
  saved_by_compression: number;
}

export interface AnalyticsTimeseriesEntry {
  day: string;
  files: number;
  bytes_virtual: number;
}

export interface AnalyticsExtensionEntry {
  extension: string;
  files: number;
  bytes_virtual: number;
}

export interface AnalyticsUploaderEntry {
  actor: string;
  files: number;
  bytes_virtual: number;
}

export interface AnalyticsRecentFile {
  name: string;
  extension: string;
  bytes_virtual: number;
  actor: string;
  at: string;
}

export interface AnalyticsOverviewResponse {
  counts: AnalyticsCounts;
  bytes: AnalyticsBytes;
  timeseries_30d: AnalyticsTimeseriesEntry[];
  top_extensions: AnalyticsExtensionEntry[];
  top_uploaders: AnalyticsUploaderEntry[];
  recent_files: AnalyticsRecentFile[];
}

export interface AnalyticsBreakdownActorRow {
  slice: string;
  files: number;
  bytes_virtual: number;
  actors: number;
}

export interface AnalyticsBreakdownQuotaRow {
  slice: string;
  files: number;
  bytes_virtual: number;
  storage_limit_bytes?: number | null;
  entry_count_limit?: number | null;
}

export interface AnalyticsBreakdownActorResponse {
  kind: string;
  rows: AnalyticsBreakdownActorRow[];
  count: number;
}

export interface AnalyticsBreakdownQuotaResponse {
  kind: string;
  rows: AnalyticsBreakdownQuotaRow[];
  count: number;
}
