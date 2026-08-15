-- ============================================================
-- storage.file_details
-- internal view — full context for a stored file, using owner columns stored
-- directly on the file row.
--
-- schema changes vs previous version:
--   - base table: storage.user_files → storage.files
--   - `full_path` is materialized on storage.files
--   - `path` column removed — use full_path from the view
--   - `status` column removed from storage.files
--   - `directory_id` added (null = file is at session root)
--   - owner identity is stored directly on storage.files, so we can left join
--     user and service metadata without traversing core.sessions.
-- ============================================================
create or replace view storage.file_details as
select
  fp.id as file_id,
  fp.name as file_name,
  fp.full_path as file_path,
  fp.directory_path,
  fp.directory_id,
  fp.created_at,
  -- compression info
  fp.is_compressed,
  fp.compression_algorithm,
  fp.compression_level,
  -- ownership context
  fp.session_id,
  fp.user_id,
  fp.automated_service_id,
  -- uploader identity — only populated for human sessions
  u.domain as domain_name,
  coalesce(u.organisation, a.organisation) as organisation_name,
  u.first_name,
  u.family_name,
  u.email,
  -- automated service identity — only populated for agent sessions
  a.name as automated_service_name,
  a.metadata as automated_service_metadata,
  -- storage object
  so.id as object_id,
  so.bucket,
  so.key,
  so.size as object_size,
  so.created_at as object_created_at,
  so.deleted_at as object_deleted_at,
  -- format
  f.extension,
  f.mime_type
from
  storage.file_paths fp
  join storage.objects so on so.id = fp.object_id
  left join core.users u on u.id = fp.user_id
  left join core.automated_services a on a.id = fp.automated_service_id
  left join storage.formats f on f.extension = fp.extension;

-- ============================================================
-- api.files
-- postgrest-facing view. excludes soft-deleted objects.
--
-- supported filter examples (postgrest query params):
--   ?session_id=eq.10
--   ?user_id=eq.5
--   ?agent_id=eq.2
--   ?directory_id=eq.7
--   ?extension=eq.pdf
--   ?full_path=like./documents/%
--   ?name=ilike.*invoice*&limit=20&offset=0
--   ?is_compressed=eq.true
--   ?compression_algorithm=eq.zstd
-- ============================================================
create or replace view postgrest.files as
select
  fp.id,
  fp.name,
  fp.full_path,
  fp.directory_path,
  fp.directory_id,
  fp.extension,
  fp.created_at,
  -- compression info
  fp.is_compressed,
  fp.compression_algorithm,
  fp.compression_level,
  -- ownership linkage
  fp.session_id,
  fp.user_id,
  fp.automated_service_id,
  u.domain,
  coalesce(u.organisation, a.organisation) as organisation,
  -- storage object details
  so.id as object_id,
  so.bucket,
  so.key,
  so.size,
  so.created_at as object_created_at,
  -- format metadata
  f.mime_type
from
  storage.file_paths fp
  join storage.objects so on so.id = fp.object_id
  left join core.users u on u.id = fp.user_id
  left join core.automated_services a on a.id = fp.automated_service_id
  left join storage.formats f on f.extension = fp.extension
where
  so.deleted_at is null;

-- ============================================================
-- postgrest.formats
-- exposes allowed file formats for upload-constraint checks.
-- ============================================================
create or replace view postgrest.formats as
select
  extension,
  mime_type,
  is_compressed,
  description
from
  storage.formats
where
  is_allowed = true;

-- ============================================================
-- grants — select only; writes go through the service layer
-- ============================================================
grant
select
  on storage.file_details to sgrn_datastore,
  sgrn_postgrest;

grant
select
  on storage.directory_tree to sgrn_datastore,
  sgrn_postgrest;

grant
select
  on postgrest.files to sgrn_datastore,
  sgrn_postgrest;

grant
select
  on postgrest.formats to sgrn_datastore,
  sgrn_postgrest;
