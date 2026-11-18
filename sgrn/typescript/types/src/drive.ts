// ==========================================
// Virtual Drive, Directory Listings & Navigation
// ==========================================

export interface DriveFolder {
  id?: number;
  name: string;
  path: string;
  created_at?: string;
  display_name?: string;
  token?: string;
  kind?: string;
  virtual_size?: number;
  real_size?: number;
  count_sub_files?: number;
  count_sub_directories?: number;
}

export interface DrivePathNode {
  id?: number | null;
  name: string;
  path: string;
  display_name?: string;
}

export interface DriveFile {
  id: number;
  name: string;
  path: string;
  extension: string;
  size: number; // Compressed size
  original_size: number;
  created_at: string;
  upload_mode?: string;
  part_count?: number | null;
  sha512?: string;
  etag?: string;
  is_encrypted?: boolean;
  key_id?: string | null;
}

export interface DirectoryListing {
  path: string;
  trail?: DrivePathNode[];
  folders: DriveFolder[];
  files: DriveFile[];
  total_items: number;
  total_folders: number;
  total_files: number;
  page: number;
  page_size: number;
  total_pages: number;
  sort?: string;
  order?: "asc" | "desc";
}

export interface Directory {
  id: number;
  parent_id?: number | null;
  user_id?: number | null;
  automated_service_id?: number | null;
  session_id?: number;
  name: string;
  path: string;
  virtual_size: number;
  real_size: number;
  count_sub_files?: number;
  count_sub_directories?: number;
  depth?: number;
  created_at?: string;
}

export interface MoveItemResponse {
  id: number;
  type: string;
  parent_id?: number | null;
  name?: string | null;
}

export interface PresignedUrlResponse {
  url: string;
  action: string;
  expiry: number;
  file_id: number;
}

export interface DeleteResponse {
  id: number;
  type: string;
}

export interface BulkResult {
  id: number;
  type: string;
  success: boolean;
  error?: string | null;
}

export interface BulkResponse {
  action: string;
  total: number;
  success_count: number;
  results: BulkResult[];
}

export interface StorageStatsResponse {
  file_count: number;
  total_original_bytes: number;
  total_compressed_bytes: number;
  storage_limit?: number | null;
}

export interface TrailNode {
  path: string;
  name: string;
  id?: number | null;
  display_name?: string | null;
}

export interface Capabilities {
  can_read: boolean;
  can_write: boolean;
  can_delete: boolean;
  allowed_subpath: string;
}

export interface GenericDriveListResponse<
  TFolder = DriveFolder,
  TFile = DriveFile,
> {
  path: string;
  trail: TrailNode[];
  folders: TFolder[];
  files: TFile[];
  capabilities: Capabilities;
  total_folders: number;
  total_files: number;
  total_items: number;
  page: number;
  page_size: number;
  total_pages: number;
  sort?: string;
  order?: string;
}
