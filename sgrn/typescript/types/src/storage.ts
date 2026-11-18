// ==========================================
// Storage, Objects, Formats & Uploads
// ==========================================

export interface StorageObject {
  id: number;
  bucket?: string;
  key?: string;
  size: number;
  original_size: number;
  is_compressed: boolean;
  compression_algorithm: string | null;
  compression_level: number | null;
  created_at?: string;
  deleted_at?: string | null;
  upload_mode?: string;
  part_count?: number | null;
  part_size_bytes?: number | null;
}

export interface FileMetadata {
  id: number;
  name: string;
  object_id: number;
  session_id: number;
  extension: string;
  directory_id?: number | null;
  created_at: string;

  // Optional Extended Schema details
  full_path?: string;
  directory_path?: string;
  user_id?: number;
  domain?: string | null;
  bucket: string;
  garage_key: string;
  size: number;
  object_created_at?: string;
  mime_type?: string | null;
  sha512?: string;
  etag?: string;
  object?: StorageObject;
}

export interface UploadResult {
  file_id?: number;
  object_id?: number;
  file?: string; // Original filename (web)
  original_filename?: string;
  virtual_path?: string;
  success?: boolean;
  status?: number;
  error?: string;
  already_exists?: boolean;
  is_duplicate?: boolean; // alias for already_exists
  deduplicated?: boolean;
  identity?: {
    key: string;
    hash: string;
    extension: string;
    mime_type: string;
    is_compressed: boolean;
    original_size: number;
    final_size: number;
    compression_algorithm?: string;
    compression_level?: number;
  };
  uploaded_at?: string;
  upload_mode?: string;
  part_count?: number | null;
  part_size_bytes?: number | null;
  original_size_bytes?: number;
  final_size_bytes?: number;
  [key: string]: unknown;
}

export interface CreateDirectoryResponse {
  directory_id?: number | null;
}

export interface UploadConstraints {
  chunk_size?: number;
  max_session_size?: number;
  max_file_size_mb?: number;
  parallel_chunks?: number;
  max_retries?: number;
  units?: string;
  version?: string;
  description?: string;
  allowed_extensions?: string[];
  compression_level?: number;
  threshold_presigned_mb?: number;
  [key: string]: unknown;
}

export interface UploadInitRequest {
  filename: string;
  target_path: string;
  scope: string;
  mime_type?: string;
  total_size: number;
  chunk_size: number;
  encrypt?: boolean;
}

export interface UploadInitResponse {
  upload_id: string;
  chunk_size: number;
  total_chunks: number;
  status: "active" | string;
  is_encrypted: boolean;
}

export interface UploadChunkResponse {
  upload_id: string;
  chunk_index: number;
  received: boolean;
  duplicate?: boolean;
  segments_flushed: number;
}

export interface UploadStatusResponse {
  upload_id: string;
  filename: string;
  target_path: string;
  total_size: number;
  chunk_size: number;
  total_chunks: number;
  uploaded_chunks_count: number;
  status: string;
  uploaded_chunk_indices: number[];
}

export interface UploadCompleteResponse {
  upload_id: string;
  file_id: number;
  file_hash: string;
  segment_count: number;
  original_bytes: number;
  is_encrypted: boolean;
  message: string;
}

export interface UploadAbortResponse {
  message: string;
}

export interface OverviewBucket {
  name: string;
  garage: {
    objects: number;
    bytes: number;
    truncated: boolean;
    pages: number;
  };
  db: {
    objects: number;
    bytes: number;
    files: number;
  };
}

export interface OverviewResponse {
  buckets: OverviewBucket[];
}

export interface OrphanObject {
  key: string;
  size: number;
  etag: string;
}

export interface OrphanSize {
  key: string;
  size: number;
}

export interface OrphansResponse {
  bucket: string;
  prefix: string;
  garage_scanned: number;
  garage_truncated: boolean;
  garage_only: OrphanObject[];
  garage_only_count: number;
  garage_only_bytes: number;
  db_missing: OrphanSize[];
  db_missing_unchecked: number;
}

export interface FormatRecord {
  extension: string;
  mime_type: string;
  is_compressed: boolean;
  is_allowed: boolean;
  description?: string | null;
  created_at: string;
}

export interface FormatsResponse {
  formats: FormatRecord[];
  count: number;
}

export interface FormatUpsertResponse extends FormatRecord {
  created: boolean;
}

export interface FormatDeleteResponse {
  extension: string;
}

export interface FormatConflictResponse {
  extension: string;
  referencing_files: number;
  hint: string;
}
