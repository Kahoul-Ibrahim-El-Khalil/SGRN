-- SGRN migration 001: MinIO -> Garage provider + chunking trace.
--
-- WHY THIS FILE EXISTS: `sgrn_datastore --init-db` DROPS and recreates the
-- whole database from the embedded schema, so fresh installs never need it.
-- This file is for EXISTING deployments upgrading their binaries: apply it
-- once, with the datastore STOPPED, before first booting the new binary.
--
--   psql -h /tmp -U <superuser> -d sgrn -v ON_ERROR_STOP=1 \
--     -f 001_garage_provider_and_chunking_trace.sql
--
-- CONVENTIONS: every migration here must be idempotent (safe to re-run):
-- conditional DML, IF NOT EXISTS DDL, CREATE OR REPLACE for functions/views.
-- Never reference machine-specific rows (keys, ids); see note below.
--
-- NOTE on backfill: pre-migration rows keep upload_mode='single' with NULL
-- counts. If you know a row was multipart-assembled (prove it: HeadObject
-- returns an ETag ending in `-N` = N parts, with the part size from the
-- threshold config live at upload time), stamp it by hand, e.g.:
--
--   UPDATE storage.objects
--   SET upload_mode='multipart', part_count=4, part_size_bytes=5242880
--   WHERE bucket='sgrn-uploads' AND key='<key>';

-- --- provider move (old binaries wrote 'MINIO'; upsert_object treats a
-- --- provider mismatch as a conflict, so rows must move with the bytes) ---
UPDATE storage.objects SET provider = 'GARAGE' WHERE provider = 'MINIO';
ALTER TABLE storage.objects ALTER COLUMN provider SET DEFAULT 'GARAGE';

-- --- chunking trace columns -------------------------------------------
ALTER TABLE storage.objects ADD COLUMN IF NOT EXISTS upload_mode text NOT NULL DEFAULT 'single';
ALTER TABLE storage.objects ADD COLUMN IF NOT EXISTS part_count int;
ALTER TABLE storage.objects ADD COLUMN IF NOT EXISTS part_size_bytes bigint;

-- --- upsert_object: trace params, mode refresh (not conflict) on match --
create or replace function storage.upsert_object (
  p_bucket text,
  p_key text,
  p_size bigint,
  p_original_size bigint,
  p_provider text default 'GARAGE',
  p_is_compressed boolean default false,
  p_compression_algorithm text default null,
  p_compression_level int default null,
  p_upload_mode text default 'single',
  p_part_count int default null,
  p_part_size_bytes bigint default null
) returns bigint language plpgsql as $$
declare
  v_id bigint;
  v_existing record;
begin
  insert into storage.objects (
    bucket,
    key,
    size,
    original_size,
    provider,
    is_compressed,
    compression_algorithm,
    compression_level,
    upload_mode,
    part_count,
    part_size_bytes
  )
  values (
    p_bucket,
    p_key,
    p_size,
    p_original_size,
    p_provider,
    p_is_compressed,
    p_compression_algorithm,
    p_compression_level,
    p_upload_mode,
    p_part_count,
    p_part_size_bytes
  )
  on conflict (bucket, key) do nothing
  returning id into v_id;

  if v_id is not null then
    return v_id;
  end if;

  select
    o.id,
    o.size,
    o.original_size,
    o.provider,
    o.is_compressed,
    o.compression_algorithm,
    o.compression_level
  into v_existing
  from storage.objects o
  where o.bucket = p_bucket and o.key = p_key
  for update;

  if v_existing.id is null then
    raise exception 'Object upsert failed for bucket=%, key=%', p_bucket, p_key;
  end if;

  if v_existing.size <> p_size
     or v_existing.original_size <> p_original_size
     or v_existing.provider is distinct from p_provider
     or v_existing.is_compressed is distinct from p_is_compressed
     or v_existing.compression_algorithm is distinct from p_compression_algorithm
     or v_existing.compression_level is distinct from p_compression_level then
    raise exception 'Conflicting metadata for existing object (bucket=%, key=%)', p_bucket, p_key
      using errcode = '23505';
  end if;

  update storage.objects
  set upload_mode = p_upload_mode,
      part_count = p_part_count,
      part_size_bytes = p_part_size_bytes
  where id = v_existing.id
    and (upload_mode is distinct from p_upload_mode
      or part_count is distinct from p_part_count
      or part_size_bytes is distinct from p_part_size_bytes);

  v_id := v_existing.id;
  return v_id;
end;
$$;

-- --- file_details: expose the trace (columns appended last: REPLACE ----
-- --- cannot reorder existing view columns) ------------------------------
create or replace view storage.file_details as
select
  fp.id as file_id,
  fp.name as file_name,
  fp.full_path as file_path,
  fp.directory_path,
  fp.directory_id,
  fp.created_at,
  fp.is_compressed,
  fp.compression_algorithm,
  fp.compression_level,
  fp.session_id,
  fp.user_id,
  fp.automated_service_id,
  u.domain as domain_name,
  coalesce(u.organisation, a.organisation) as organisation_name,
  u.first_name,
  u.family_name,
  u.email,
  a.name as automated_service_name,
  a.metadata as automated_service_metadata,
  so.id as object_id,
  so.bucket,
  so.key,
  so.size as object_size,
  so.created_at as object_created_at,
  so.deleted_at as object_deleted_at,
  f.extension,
  f.mime_type,
  so.upload_mode,
  so.part_count,
  so.part_size_bytes
from
  storage.file_paths fp
  join storage.objects so on so.id = fp.object_id
  left join core.users u on u.id = fp.user_id
  left join core.automated_services a on a.id = fp.automated_service_id
  left join storage.formats f on f.extension = fp.extension;

-- --- verify -------------------------------------------------------------
SELECT provider, upload_mode, part_count, COUNT(*)
FROM storage.objects GROUP BY provider, upload_mode, part_count ORDER BY 1, 2, 3;
