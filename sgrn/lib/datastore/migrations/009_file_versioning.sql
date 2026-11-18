-- ============================================================================
-- Migration 009: File versioning with reverse deltas
--
-- Model
--   * The HEAD version of a file is always stored in full. storage.file_objects
--     keeps mirroring the head, so downloads, quotas, directory sizes and
--     storage.file_details keep working untouched.
--   * Older versions are reverse deltas stored as Garage blobs:
--       storage.file_deltas.version_id1 = BASE   (the newer version)
--       storage.file_deltas.version_id2 = TARGET (the older version)
--     Applying the delta blob to the content of version_id1 yields version_id2.
--   * Every version that is not the head and not a checkpoint is eventually
--     compacted to 'delta' by a background worker (never on the upload path).
--     Every `snapshot_interval`-th version stays 'full' (checkpoint) so a
--     restore never walks more than snapshot_interval deltas.
--   * Delta contents are computed over LOGICAL bytes: decrypted and
--     decompressed. Ciphertext / compressed streams do not delta.
--
-- Idempotent. No begin/commit (runner conventions: see migrations/README.md).
-- Also fold this file into postgres/schemas/storage.sql AFTER
-- storage.recompute_actor_usage, because --init-db builds from there.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 1. storage.file_versions
-- ----------------------------------------------------------------------------
create table if not exists storage.file_versions (
  id                        bigint generated always as identity primary key,
  file_id                   bigint  not null references storage.files (id) on delete cascade,
  version_no                int     not null check (version_no > 0),
  storage_kind              text    not null default 'full'
                              check (storage_kind in ('full', 'delta')),
  is_checkpoint             boolean not null default false,
  content_sha256            char(64) not null,          -- logical bytes (plaintext, uncompressed)
  original_size             bigint  not null check (original_size >= 0),
  is_encrypted              boolean not null default false,
  key_id                    text    default null,       -- keys.db reference, never key material
  comment                   text    default null,
  restored_from_version_id  bigint  default null references storage.file_versions (id) on delete set null,
  created_by_user_id        int     default null references core.users (id) on delete set null,
  created_by_service_id     int     default null references core.automated_services (id) on delete set null,
  created_at                timestamptz not null default now(),
  unique (file_id, version_no),
  unique (id, file_id),
  constraint chk_versions_encryption_consistency check (
    (is_encrypted = false and key_id is null)
    or (is_encrypted = true and key_id is not null)
  )
);

create index if not exists idx_file_versions_file
  on storage.file_versions (file_id, version_no desc);

create unique index if not exists uq_file_versions_key_id
  on storage.file_versions (key_id) where key_id is not null;

-- Work queue for the compactor: full, not head-protected here (head is
-- excluded in the query by comparing with files.current_version_id).
create index if not exists idx_file_versions_compactable
  on storage.file_versions (file_id, version_no)
  where storage_kind = 'full' and not is_checkpoint;

-- ----------------------------------------------------------------------------
-- 2. storage.version_objects  (Garage objects of 'full' versions)
-- ----------------------------------------------------------------------------
create table if not exists storage.version_objects (
  version_id bigint not null references storage.file_versions (id) on delete cascade,
  object_id  bigint not null references storage.objects (id) on delete restrict,
  part_index int    not null default 0,
  role       text   not null default 'primary' check (role in ('primary', 'part')),
  primary key (version_id, part_index)
);

create index if not exists idx_version_objects_object
  on storage.version_objects (object_id);

-- ----------------------------------------------------------------------------
-- 3. storage.file_deltas  (reverse deltas, blob lives in Garage)
-- ----------------------------------------------------------------------------
create table if not exists storage.file_deltas (
  id          bigint generated always as identity primary key,
  file_id     bigint  not null,
  version_id1 bigint  not null,                 -- BASE   (newer)
  version_id2 bigint  not null,                 -- TARGET (older)
  object_id   bigint  not null references storage.objects (id) on delete restrict,
  hash        char(64) not null,                -- sha256 of the stored delta blob
  algorithm   text    not null default 'zstd-prefix' check (algorithm in ('zstd-prefix')),
  delta_size  bigint  not null check (delta_size > 0),
  created_at  timestamptz not null default now(),
  constraint fk_delta_base   foreign key (version_id1, file_id)
    references storage.file_versions (id, file_id) on delete cascade,
  constraint fk_delta_target foreign key (version_id2, file_id)
    references storage.file_versions (id, file_id) on delete cascade,
  constraint chk_delta_distinct check (version_id1 <> version_id2),
  unique (version_id2)                          -- a delta-kind version has exactly one incoming delta
);

create index if not exists idx_file_deltas_base   on storage.file_deltas (version_id1);
create index if not exists idx_file_deltas_object on storage.file_deltas (object_id);

-- ----------------------------------------------------------------------------
-- 4. storage.files: head pointer + counter
-- ----------------------------------------------------------------------------
alter table storage.files add column if not exists current_version_id bigint;
alter table storage.files add column if not exists version_count int not null default 0;

do $$
begin
  if not exists (
    select 1 from pg_constraint
    where conname = 'fk_files_current_version'
      and connamespace = 'storage'::regnamespace
  ) then
    alter table storage.files
      add constraint fk_files_current_version
      foreign key (current_version_id) references storage.file_versions (id)
      on delete set null deferrable initially deferred;
  end if;
end
$$;

-- ----------------------------------------------------------------------------
-- 5. Object lifetime: an object is live while ANY table references it
--    (the old trigger only looked at file_objects, and would now hit the
--    ON DELETE RESTRICT of version_objects / file_deltas).
-- ----------------------------------------------------------------------------
create or replace function storage.delete_object_if_unreferenced (p_object_id bigint)
returns void language plpgsql as $$
begin
  if p_object_id is null then
    return;
  end if;
  if not exists (select 1 from storage.file_objects  where object_id = p_object_id)
     and not exists (select 1 from storage.version_objects where object_id = p_object_id)
     and not exists (select 1 from storage.file_deltas     where object_id = p_object_id) then
    delete from storage.objects where id = p_object_id;
  end if;
end;
$$;

create or replace function storage.cleanup_orphan_objects () returns trigger language plpgsql as $$
begin
  perform storage.delete_object_if_unreferenced(old.object_id);
  if tg_op = 'UPDATE' then
    return new;
  end if;
  return old;
end;
$$;

create or replace function storage.cleanup_orphan_objects_versions () returns trigger language plpgsql as $$
begin
  perform storage.delete_object_if_unreferenced(old.object_id);
  return old;
end;
$$;

drop trigger if exists trg_cleanup_orphan_version_objects on storage.version_objects;
create trigger trg_cleanup_orphan_version_objects
after update of object_id or delete on storage.version_objects
for each row execute function storage.cleanup_orphan_objects_versions ();

drop trigger if exists trg_cleanup_orphan_delta_objects on storage.file_deltas;
create trigger trg_cleanup_orphan_delta_objects
after delete on storage.file_deltas
for each row execute function storage.cleanup_orphan_objects_versions ();

-- ----------------------------------------------------------------------------
-- 6. Quota accounting: history counts against the actor (virtual + real).
--    History = non-head 'full' snapshots (original_size / size) + delta blobs
--    (size). Head bytes are already counted through file_objects.
--    NOTE: quota gates compare total_virtual_size, so history is gated too.
-- ----------------------------------------------------------------------------
create or replace function storage.history_bytes_for_files (p_file_ids bigint[])
returns table (virtual_size bigint, real_size bigint)
language sql stable as $$
  select
    coalesce(sum(h.v), 0)::bigint,
    coalesce(sum(h.r), 0)::bigint
  from (
    select o.original_size as v, o.size as r
    from storage.files f
    join storage.file_versions fv on fv.file_id = f.id
    join storage.version_objects vo on vo.version_id = fv.id
    join storage.objects o on o.id = vo.object_id
    where f.id = any (p_file_ids)
      and fv.id is distinct from f.current_version_id
    union all
    select d.delta_size, d.delta_size
    from storage.file_deltas d
    where d.file_id = any (p_file_ids)
  ) h
$$;

create or replace function storage.recompute_actor_usage (
  p_user_id int,
  p_service_id int
) returns void language plpgsql as $$
declare
  v_files bigint[];
  v_hist_virtual bigint;
  v_hist_real bigint;
begin
  if p_user_id is not null then
    select coalesce(array_agg(id), '{}') into v_files from storage.files where user_id = p_user_id;
    select virtual_size, real_size into v_hist_virtual, v_hist_real
      from storage.history_bytes_for_files(v_files);

    update core.users
    set total_virtual_size = (
          select coalesce(sum(o.original_size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.user_id = p_user_id
        ) + v_hist_virtual,
        total_real_size = (
          select coalesce(sum(o.size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.user_id = p_user_id
        ) + v_hist_real,
        total_entry_count = (
          select count(*)::bigint from storage.files f where f.user_id = p_user_id
        ) + (
          select count(*)::bigint from storage.directories d where d.user_id = p_user_id
        )
    where id = p_user_id;
  elsif p_service_id is not null then
    select coalesce(array_agg(id), '{}') into v_files from storage.files where automated_service_id = p_service_id;
    select virtual_size, real_size into v_hist_virtual, v_hist_real
      from storage.history_bytes_for_files(v_files);

    update core.automated_services
    set total_virtual_size = (
          select coalesce(sum(o.original_size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.automated_service_id = p_service_id
        ) + v_hist_virtual,
        total_real_size = (
          select coalesce(sum(o.size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.automated_service_id = p_service_id
        ) + v_hist_real,
        total_entry_count = (
          select count(*)::bigint from storage.files f where f.automated_service_id = p_service_id
        ) + (
          select count(*)::bigint from storage.directories d where d.automated_service_id = p_service_id
        )
    where id = p_service_id;
  end if;
end;
$$;

-- Keep counters true when history rows change (no gates here: the byte gate
-- fires on the file_objects mapping of the new head, see apply_object_owner_usage).
create or replace function storage.apply_version_usage () returns trigger language plpgsql as $$
declare
  v_file_id bigint;
  v_user_id int;
  v_service_id int;
  v_org text;
begin
  if tg_table_name = 'version_objects' then
    select fv.file_id into v_file_id from storage.file_versions fv
      where fv.id = coalesce(new.version_id, old.version_id);
  else
    v_file_id := coalesce(new.file_id, old.file_id);
  end if;

  select f.user_id, f.automated_service_id into v_user_id, v_service_id
  from storage.files f where f.id = v_file_id;
  if not found then
    return coalesce(new, old);   -- file is being deleted
  end if;

  if v_user_id is not null then
    perform storage.recompute_actor_usage(v_user_id, null);
    select organisation into v_org from core.users where id = v_user_id;
  elsif v_service_id is not null then
    perform storage.recompute_actor_usage(null, v_service_id);
    select organisation into v_org from core.automated_services where id = v_service_id;
  end if;
  if v_org is not null then
    perform core.sync_org_totals_for(v_org);
  end if;
  return coalesce(new, old);
end;
$$;

drop trigger if exists trg_apply_version_objects_usage on storage.version_objects;
create trigger trg_apply_version_objects_usage
after insert or delete on storage.version_objects
for each row execute function storage.apply_version_usage ();

drop trigger if exists trg_apply_file_deltas_usage on storage.file_deltas;
create trigger trg_apply_file_deltas_usage
after insert or delete on storage.file_deltas
for each row execute function storage.apply_version_usage ();

create or replace function storage.apply_current_version_usage () returns trigger language plpgsql as $$
declare
  v_org text;
begin
  if old.current_version_id is distinct from new.current_version_id then
    perform storage.recompute_actor_usage(new.user_id, new.automated_service_id);
    if new.user_id is not null then
      select organisation into v_org from core.users where id = new.user_id;
    elsif new.automated_service_id is not null then
      select organisation into v_org from core.automated_services where id = new.automated_service_id;
    end if;
    if v_org is not null then
      perform core.sync_org_totals_for(v_org);
    end if;
  end if;
  return new;
end;
$$;

drop trigger if exists trg_apply_current_version_usage on storage.files;
create trigger trg_apply_current_version_usage
after update of current_version_id on storage.files
for each row execute function storage.apply_current_version_usage ();

-- ----------------------------------------------------------------------------
-- 7. Functions used by the datastore
-- ----------------------------------------------------------------------------

-- 7a. Called by insertFile BEFORE it swaps file_objects on an overwrite.
--     Lazily adopts files that predate versioning (their head becomes v1).
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

  select coalesce(v_file.content_sha256, (
           select o.sha256 from storage.file_objects fo
           join storage.objects o on o.id = fo.object_id
           where fo.file_id = p_file_id and fo.part_index = 0)),
         coalesce((
           select sum(o.original_size) from storage.file_objects fo
           join storage.objects o on o.id = fo.object_id
           where fo.file_id = p_file_id), 0)
    into v_sha, v_size;

  if v_sha is null then
    raise exception 'snapshot_head_as_version: file % has no content', p_file_id;
  end if;

  insert into storage.file_versions
    (file_id, version_no, storage_kind, is_checkpoint, content_sha256, original_size,
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

-- 7b. Called AFTER file_objects now maps the new head. Registers the new
--     version. Returns created=false when the bytes did not change.
create or replace function storage.commit_new_version (
  p_file_id bigint,
  p_content_sha256 text,
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

  if v_prev_id is not null and v_prev.content_sha256 = p_content_sha256 then
    return query select v_prev.id, v_prev.version_no, v_prev.id, false;
    return;
  end if;

  select coalesce(max(x.version_no), 0) + 1 into v_next
  from storage.file_versions x where x.file_id = p_file_id;

  insert into storage.file_versions
    (file_id, version_no, storage_kind, is_checkpoint, content_sha256, original_size,
     is_encrypted, key_id, comment, restored_from_version_id,
     created_by_user_id, created_by_service_id)
  values
    (p_file_id, v_next, 'full',
     p_is_checkpoint or (p_snapshot_interval > 0 and v_next % p_snapshot_interval = 0),
     p_content_sha256, p_original_size,
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
      content_sha256 = p_content_sha256
  where id = p_file_id;

  return query select v_id, v_next, v_prev_id, true;
end;
$$;

-- 7c. Compactor: turn a 'full' non-head version into a delta of its newer
--     neighbour. Verifies adjacency so chains stay linear.
create or replace function storage.attach_delta (
  p_base_version_id bigint,
  p_target_version_id bigint,
  p_object_id bigint,
  p_hash text,
  p_delta_size bigint
) returns bigint language plpgsql as $$
declare
  v_base storage.file_versions%rowtype;
  v_target storage.file_versions%rowtype;
  v_head bigint;
  v_delta_id bigint;
begin
  select * into v_target from storage.file_versions where id = p_target_version_id for update;
  select * into v_base   from storage.file_versions where id = p_base_version_id   for update;
  if v_target.id is null or v_base.id is null or v_target.file_id <> v_base.file_id then
    raise exception 'attach_delta: versions do not belong to the same file';
  end if;
  if v_base.version_no <> v_target.version_no + 1 then
    raise exception 'attach_delta: versions % and % are not adjacent', v_base.version_no, v_target.version_no;
  end if;
  if v_target.storage_kind <> 'full' or v_target.is_checkpoint then
    raise exception 'attach_delta: target version % is not compactable', v_target.version_no;
  end if;
  select current_version_id into v_head from storage.files where id = v_target.file_id;
  if v_head = v_target.id then
    raise exception 'attach_delta: the head version is never compacted';
  end if;

  insert into storage.file_deltas (file_id, version_id1, version_id2, object_id, hash, delta_size)
  values (v_target.file_id, v_base.id, v_target.id, p_object_id, p_hash, p_delta_size)
  returning id into v_delta_id;

  update storage.file_versions set storage_kind = 'delta' where id = v_target.id;
  delete from storage.version_objects where version_id = v_target.id;   -- frees the full copy
  return v_delta_id;
end;
$$;

-- 7d. Reconstruction plan. Rows come back in application order (step 0 first):
--     step 0 is the FULL base (head or checkpoint), the last row is the
--     requested version. A row's delta_* columns describe the delta that must
--     be applied to THAT row's content to obtain the NEXT row (null on the last
--     row). Delta blobs are encrypted with the key of the version they produce
--     (delta_key_id), so they stay unreadable once that version is trimmed.
create or replace function storage.version_plan (p_version_id bigint)
returns table (
  step int, version_id bigint, storage_kind text,
  delta_id bigint, delta_object_key text, delta_hash text, delta_size bigint,
  delta_is_encrypted boolean, delta_key_id text,
  original_size bigint, content_sha256 text
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
    v.original_size, v.content_sha256::text
  from walk w
  join storage.file_versions v on v.id = w.vid
  left join storage.file_deltas d on d.id = w.via_delta
  left join storage.objects ob on ob.id = d.object_id
  left join storage.file_versions t on t.id = d.version_id2
  order by w.depth desc
$$;

-- 7e. Retention: drop the OLDEST versions only (keeps every chain valid:
--     older versions are never a base of newer ones). Returns the key_ids (to
--     crypto-shred) and the Garage keys of the dropped versions' blobs so the
--     caller can delete the ones whose storage.objects row is now gone.
create or replace function storage.trim_versions (
  p_file_id bigint,
  p_keep int,
  p_older_than interval default null
) returns table (version_id bigint, key_id text, blob_keys text[])
language plpgsql as $$
begin
  if p_keep < 1 then
    raise exception 'trim_versions: keep must be >= 1';
  end if;
  return query
  with doomed as (
    select fv.id as vid, fv.key_id as vkey,
           array(
             select ob.key from storage.version_objects vo
               join storage.objects ob on ob.id = vo.object_id where vo.version_id = fv.id
             union all
             select ob.key from storage.file_deltas d
               join storage.objects ob on ob.id = d.object_id where d.version_id2 = fv.id
           ) as vkeys
    from storage.file_versions fv
    where fv.file_id = p_file_id
      and fv.version_no <= (
        select coalesce(max(x.version_no), 0) - p_keep from storage.file_versions x where x.file_id = p_file_id)
      and (p_older_than is null or fv.created_at < now() - p_older_than)
      and fv.id is distinct from (select f.current_version_id from storage.files f where f.id = p_file_id)
  ), gone as (
    delete from storage.file_versions fv using doomed d where fv.id = d.vid
    returning d.vid, d.vkey, d.vkeys
  )
  select g.vid, g.vkey, g.vkeys from gone g;

  update storage.files f
  set version_count = (select count(*) from storage.file_versions x where x.file_id = f.id)
  where f.id = p_file_id;
end;
$$;

-- ----------------------------------------------------------------------------
-- 8. Listing view
-- ----------------------------------------------------------------------------
create or replace view storage.file_version_details as
select
  v.id as version_id,
  v.file_id,
  v.version_no,
  v.storage_kind,
  v.is_checkpoint,
  v.content_sha256,
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

-- ----------------------------------------------------------------------------
-- Verify
-- ----------------------------------------------------------------------------
select
  (select count(*) from storage.file_versions) as versions,
  (select count(*) from storage.file_deltas)   as deltas,
  (select count(*) from storage.files where current_version_id is null) as files_not_yet_versioned;
