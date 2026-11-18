-- ============================================================================
-- Migration 010: SHA-512 content hashes
--
-- WHY: SGRN content/identity hashes moved from SHA-256 (64 hex chars) to
-- SHA-512 (128 hex chars). This covers storage.objects checksums, version
-- content hashes and reverse-delta blob hashes. Protocol-defined hashes stay
-- on SHA-256: JWT HS256, webhook HMAC, AWS SigV4 and the tamper-evident audit
-- chain (which cannot be rehashed without invalidating every existing row).
--
-- NOTE on existing data: a SHA-512 digest cannot be derived from a stored
-- SHA-256 digest. The legacy sha256 columns are kept for forensics, while the
-- new sha512 columns start empty. Existing objects therefore have to be
-- re-uploaded (or re-indexed by an external tool that can read every object)
-- before they participate in checksum verification, delta compaction and
-- backup verification. New writes populate the SHA-512 columns normally.
--
-- Idempotent. No begin/commit (runner conventions: see migrations/README.md).
-- Also fold this file into postgres/schemas/storage.sql.
-- ============================================================================

-- --- storage.objects.sha512 ------------------------------------------------
alter table storage.objects
  add column if not exists sha512 varchar(88) not null default '';

-- --- storage.files.content_sha512 -----------------------------------------
alter table storage.files
  add column if not exists content_sha512 text default null;

-- --- storage.file_versions.content_sha512 ----------------------------------
alter table storage.file_versions
  add column if not exists content_sha512 varchar(88) not null default '';

-- Legacy SHA-256 columns stay behind so old values remain inspectable. They
-- are no longer written by the application, so give the NOT NULL ones a
-- default (inserts omit them from now on). Guarded because a database created
-- from the current schema has no legacy columns at all.
do $$
begin
  if exists (
    select 1 from information_schema.columns
    where table_schema = 'storage' and table_name = 'objects' and column_name = 'sha256'
  ) then
    alter table storage.objects alter column sha256 set default '';
  end if;
  if exists (
    select 1 from information_schema.columns
    where table_schema = 'storage' and table_name = 'file_versions' and column_name = 'content_sha256'
  ) then
    alter table storage.file_versions alter column content_sha256 set default '';
  end if;
end;
$$;

-- --- storage.file_deltas.hash ---------------------------------------------
alter table storage.file_deltas
  alter column hash type char(128);

-- ----------------------------------------------------------------------------
-- Functions: recreate with the SHA-512 column names.
-- ----------------------------------------------------------------------------
-- CREATE OR REPLACE cannot rename input parameters (commit_new_version's
-- p_content_sha256 → p_content_sha512) or change the OUT row of a set-returning
-- function (version_plan's content_sha256 → content_sha512). Drop those
-- signatures first. upsert_object is dropped as well so a deployment that
-- still carries the pre-002 signature cannot end up with two overloads.
drop function if exists storage.upsert_object(
  text, text, bigint, bigint, character, text, boolean, text, integer, text, integer, bigint);
-- Pre-002 signature (no hash parameter). Leaving it behind makes 5-argument
-- calls such as the compactor's `upsert_object(bucket, key, size, orig, hash)`
-- resolve to it and store the hash as `provider`.
drop function if exists storage.upsert_object(
  text, text, bigint, bigint, text, boolean, text, integer, text, integer, bigint);
drop function if exists storage.commit_new_version(
  bigint, text, bigint, boolean, text, boolean, int, int, text, bigint, int);
drop function if exists storage.version_plan(bigint);

create or replace function storage.upsert_object (
  p_bucket text,
  p_key text,
  p_size bigint,
  p_original_size bigint,
  p_sha512 varchar(88),
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
    sha512,
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
    p_sha512,
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
    o.sha512,
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
     or v_existing.sha512 <> p_sha512
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

  return v_existing.id;
end;
$$;

create or replace function storage.snapshot_head_as_version (
  p_file_id bigint,
  p_user_id int default null,
  p_service_id int default null
) returns bigint language plpgsql as $$
declare
  v_file storage.files%rowtype;
  v_version_id bigint;
  v_sha text;
  v_size bigint;
begin
  select * into v_file from storage.files where id = p_file_id for update;
  if not found then
    raise exception 'snapshot_head_as_version: file % not found', p_file_id;
  end if;
  if v_file.current_version_id is not null then
    return v_file.current_version_id;
  end if;

  select coalesce(nullif(v_file.content_sha512, ''), (
           select o.sha512 from storage.file_objects fo
           join storage.objects o on o.id = fo.object_id
           where fo.file_id = p_file_id and fo.part_index = 0)),
         coalesce((
           select sum(o.original_size) from storage.file_objects fo
           join storage.objects o on o.id = fo.object_id
           where fo.file_id = p_file_id), 0)
    into v_sha, v_size;

  if v_sha is null or v_sha = '' then
    raise exception 'snapshot_head_as_version: file % has no content hash', p_file_id;
  end if;

  insert into storage.file_versions
    (file_id, version_no, storage_kind, is_checkpoint, content_sha512, original_size,
     is_encrypted, key_id, created_by_user_id, created_by_service_id, created_at)
  values
    (p_file_id, 1, 'full', false, v_sha, v_size,
     v_file.is_encrypted, v_file.key_id, p_user_id, p_service_id, v_file.created_at)
  returning id into v_version_id;

  insert into storage.version_objects (version_id, object_id, part_index, role)
  select v_version_id, fo.object_id, fo.part_index, case when fo.part_index = 0 then 'primary' else 'part' end
  from storage.file_objects fo
  where fo.file_id = p_file_id;

  update storage.files
  set current_version_id = v_version_id, version_count = 1
  where id = p_file_id;

  return v_version_id;
end;
$$;

create or replace function storage.commit_new_version (
  p_file_id bigint,
  p_content_sha512 text,
  p_original_size bigint,
  p_is_encrypted boolean,
  p_key_id text,
  p_is_checkpoint boolean default false,
  p_user_id int default null,
  p_service_id int default null,
  p_comment text default null,
  p_restored_from bigint default null,
  p_snapshot_interval int default 10
) returns table (version_id bigint, version_no int, prev_version_id bigint, created boolean)
language plpgsql as $$
declare
  v_prev storage.file_versions%rowtype;
  v_prev_id bigint;
  v_next int;
  v_id bigint;
begin
  perform 1 from storage.files where id = p_file_id for update;

  select fv.* into v_prev
  from storage.files f join storage.file_versions fv on fv.id = f.current_version_id
  where f.id = p_file_id;
  v_prev_id := v_prev.id;

  if v_prev_id is not null and v_prev.content_sha512 = p_content_sha512 then
    return query select v_prev.id, v_prev.version_no, v_prev.id, false;
    return;
  end if;

  select coalesce(max(x.version_no), 0) + 1 into v_next
  from storage.file_versions x where x.file_id = p_file_id;

  insert into storage.file_versions
    (file_id, version_no, storage_kind, is_checkpoint, content_sha512, original_size,
     is_encrypted, key_id, comment, restored_from_version_id,
     created_by_user_id, created_by_service_id)
  values
    (p_file_id, v_next, 'full',
     p_is_checkpoint or (p_snapshot_interval > 0 and v_next % p_snapshot_interval = 0),
     p_content_sha512, p_original_size,
     p_is_encrypted, p_key_id, p_comment, p_restored_from,
     p_user_id, p_service_id)
  returning id into v_id;

  insert into storage.version_objects (version_id, object_id, part_index, role)
  select v_id, fo.object_id, fo.part_index, case when fo.part_index = 0 then 'primary' else 'part' end
  from storage.file_objects fo
  where fo.file_id = p_file_id;

  update storage.files
  set current_version_id = v_id,
      version_count = version_count + 1,
      content_sha512 = p_content_sha512
  where id = p_file_id;

  return query select v_id, v_next, v_prev_id, true;
end;
$$;

create or replace function storage.version_plan (p_version_id bigint)
returns table (
  step int, version_id bigint, storage_kind text,
  delta_id bigint, delta_object_key text, delta_hash text, delta_size bigint,
  delta_is_encrypted boolean, delta_key_id text,
  original_size bigint, content_sha512 text
) language sql stable as $$
  with recursive walk as (
    select v.id as vid, 0 as depth, null::bigint as via_delta
    from storage.file_versions v where v.id = p_version_id
    union all
    select d.version_id1, w.depth + 1, d.id
    from walk w
    join storage.file_versions cur on cur.id = w.vid and cur.storage_kind = 'delta'
    join storage.file_deltas d on d.version_id2 = w.vid
    where w.depth < 100000
  )
  select
    (max(w.depth) over () - w.depth)::int,
    v.id, v.storage_kind,
    w.via_delta, ob.key, d.hash::text, d.delta_size,
    t.is_encrypted, t.key_id,
    v.original_size, v.content_sha512::text
  from walk w
  join storage.file_versions v on v.id = w.vid
  left join storage.file_deltas d on d.id = w.via_delta
  left join storage.objects ob on ob.id = d.object_id
  left join storage.file_versions t on t.id = d.version_id2
  order by w.depth desc
$$;

drop view if exists storage.file_version_details;
create view storage.file_version_details as
select
  v.id as version_id,
  v.file_id,
  v.version_no,
  v.storage_kind,
  v.is_checkpoint,
  v.content_sha512,
  v.original_size,
  v.is_encrypted,
  v.comment,
  v.restored_from_version_id,
  v.created_by_user_id,
  v.created_by_service_id,
  v.created_at,
  (v.id = f.current_version_id) as is_current,
  case
    when v.storage_kind = 'full' then coalesce((
      select sum(o.size) from storage.version_objects vo
      join storage.objects o on o.id = vo.object_id where vo.version_id = v.id), 0)
    else coalesce((select d.delta_size from storage.file_deltas d where d.version_id2 = v.id), 0)
  end::bigint as stored_size
from storage.file_versions v
join storage.files f on f.id = v.file_id;

-- storage.file_details exposes storage.objects.sha512; recreate it so the
-- column rename is reflected (CREATE OR REPLACE cannot rename a column).
drop view if exists storage.file_details;
create view storage.file_details as
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
  so.sha512,
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

grant select on storage.file_details to sgrn_datastore;

-- ----------------------------------------------------------------------------
-- Verify
-- ----------------------------------------------------------------------------
select
  (select count(*) from storage.file_versions where content_sha512 = '') as versions_awaiting_rehash,
  (select count(*) from storage.objects where sha512 = '')       as objects_awaiting_rehash;
