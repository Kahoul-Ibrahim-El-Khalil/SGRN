-- ============================================================
-- schema
-- ============================================================
create schema if not exists storage;

-- ============================================================
-- formats
-- ============================================================
create table if not exists storage.formats (
  id int generated always as identity primary key,
  extension varchar(15) not null unique check (extension = lower(extension)),
  mime_type varchar(128) not null,
  is_compressed boolean not null default false,
  is_allowed boolean not null default true,
  description text,
  created_at timestamptz not null default now()
);

-- ============================================================
-- objects  (raw garage records)
-- ============================================================
create table if not exists storage.objects (
  id bigint generated always as identity primary key,
  bucket text not null,
  key text not null,
  sha256 char(64) not null,
  size bigint not null check (size > 0),
  original_size bigint not null check (original_size > 0),
  is_compressed boolean not null default false,
  compression_algorithm text default null,
  compression_level int default null,
  constraint chk_objects_compression_level check (
    compression_algorithm is null
    or (
      (
        compression_algorithm = 'gzip'
        and compression_level between 1 and 9
      )
      or (
        compression_algorithm = 'zstd'
        and compression_level between 1 and 22
      )
      or (
        compression_algorithm = 'brotli'
        and compression_level between 0 and 11
      )
      or (
        compression_algorithm = 'lz4'
        and compression_level between 0 and 16
      )
      or (
        compression_algorithm = 'xz'
        and compression_level between 0 and 9
      )
      or (
        compression_algorithm = 'bzip2'
        and compression_level between 1 and 9
      )
    )
  ),
  constraint chk_objects_compression_consistency check (
    (
      is_compressed = false
      and compression_algorithm is null
      and compression_level is null
    )
    or (
      is_compressed = true
      and compression_algorithm is not null
    )
  ),
  provider text not null default 'GARAGE',
  -- chunking trace: which S3 path the bytes took ("single" or "multipart").
  -- Refreshed on re-upload (see upsert_object); NULL counts mean "not multipart".
  upload_mode text not null default 'single',
  part_count int default null,
  part_size_bytes bigint default null,
  deleted_at timestamptz default null,
  created_at timestamptz not null default now(),
  unique (bucket, key)
);

alter table if exists storage.objects
add column if not exists is_compressed boolean not null default false;

alter table if exists storage.objects
add column if not exists compression_algorithm text;

alter table if exists storage.objects
add column if not exists compression_level int;

alter table if exists storage.objects
add column if not exists upload_mode text not null default 'single';

alter table if exists storage.objects
add column if not exists part_count int;

alter table if exists storage.objects
add column if not exists part_size_bytes bigint;

alter table if exists storage.objects
add column if not exists sha256 char(64) not null default '';

do $$
begin
  if not exists (
    select 1
    from pg_constraint
    where conname = 'chk_objects_compression_level'
      and connamespace = 'storage'::regnamespace
  ) then
    alter table storage.objects
      add constraint chk_objects_compression_level check (
        compression_algorithm is null
        or (
          (compression_algorithm = 'gzip'   and compression_level between 1  and 9)  or
          (compression_algorithm = 'zstd'   and compression_level between 1  and 22) or
          (compression_algorithm = 'brotli' and compression_level between 0  and 11) or
          (compression_algorithm = 'lz4'    and compression_level between 0  and 16) or
          (compression_algorithm = 'xz'     and compression_level between 0  and 9)  or
          (compression_algorithm = 'bzip2'  and compression_level between 1  and 9)
        )
      );
  end if;

  if not exists (
    select 1
    from pg_constraint
    where conname = 'chk_objects_compression_consistency'
      and connamespace = 'storage'::regnamespace
  ) then
    alter table storage.objects
      add constraint chk_objects_compression_consistency check (
        (is_compressed = false and compression_algorithm is null and compression_level is null)
        or (is_compressed = true and compression_algorithm is not null)
      );
  end if;
end
$$;

-- ============================================================
-- directories  (self-referencing tree)
-- ============================================================
create table if not exists storage.directories (
  id bigint generated always as identity primary key,
  user_id int references core.users (id) on delete cascade,
  automated_service_id int references core.automated_services (id) on delete cascade,
  session_id bigint not null references core.sessions (id) on delete cascade,
  parent_id bigint default null references storage.directories (id) on delete cascade,
  name text not null,
  path text not null,
  domain text default null,
  domain_id int references core.domains (id) on delete restrict,
  count_sub_directories int not null default 0,
  count_sub_files int not null default 0,
  virtual_size bigint not null default 0,
  real_size bigint not null default 0,
  created_at timestamptz not null default now(),
  check (
    (
      parent_id is null
      and path = '/' || name
    )
    or (parent_id is not null)
  ),
  check (
    (
      user_id is not null
      and automated_service_id is null
    )
    or (
      user_id is null
      and automated_service_id is not null
    )
    or (
      domain is not null
      and user_id is null
      and automated_service_id is null
    )
  )
);

alter table if exists storage.directories
add column if not exists virtual_size bigint not null default 0;

alter table if exists storage.directories
add column if not exists real_size bigint not null default 0;

create unique index if not exists uq_directories_root_user on storage.directories (user_id, path)
where
  user_id is not null;

create unique index if not exists uq_directories_root_service on storage.directories (automated_service_id, path)
where
  automated_service_id is not null;

create unique index if not exists uq_directories_child_user on storage.directories (user_id, parent_id, name)
where
  user_id is not null;

create unique index if not exists uq_directories_child_service on storage.directories (automated_service_id, parent_id, name)
where
  automated_service_id is not null;

-- ============================================================
-- ============================================================
-- files
-- ============================================================
create table if not exists storage.files (
  id bigint generated always as identity primary key,
  name text not null,
  user_id int references core.users (id) on delete cascade,
  automated_service_id int references core.automated_services (id) on delete cascade,
  session_id bigint not null references core.sessions (id) on delete cascade,
  domain text default null,
  domain_id int references core.domains (id) on delete restrict,
  directory_id bigint default null references storage.directories (id) on delete cascade,
  check (
    (
      user_id is not null
      and automated_service_id is null
    )
    or (
      user_id is null
      and automated_service_id is not null
    )
    or (
      domain is not null
      and user_id is null
      and automated_service_id is null
    )
  ),
  -- Legacy 1:1 FK — kept nullable for zero-downtime migration.
  -- Will be dropped once storage.file_objects is backfilled (see migration below).
  object_id bigint references storage.objects (id) on delete restrict,
  extension varchar(15) references storage.formats (extension) on update cascade,
  full_path text not null,
  -- Encryption metadata: key_id references keys.db (never Postgres), is_encrypted
  -- is false for plaintext files and true for files encrypted with the key store.
  -- No key material is stored here; key bytes live in $SGRN_DATA_DIR/keys/keys.db.
  key_id text default null,
  is_encrypted boolean not null default false,
  content_sha256 text default null,
  constraint chk_files_encryption_consistency check (
    (is_encrypted = false and key_id is null)
    or (is_encrypted = true and key_id is not null)
  ),
  created_at timestamptz not null default now()
);

-- ============================================================
-- file_objects  (1-N file → Garage object / segment manifest)
-- ============================================================
-- Each row maps one file to one Garage S3 object (one segment).
-- A single-object file has exactly one row (part_index=0, role='primary').
-- A multi-segment file has N rows ordered by part_index (role='part').
-- Deduplication is expressed by multiple file rows sharing the same object_id.
create table if not exists storage.file_objects (
  id         bigserial primary key,
  file_id    bigint not null references storage.files (id) on delete cascade,
  object_id  bigint not null references storage.objects (id) on delete restrict,
  part_index int    not null default 0,
  role       text   not null default 'primary'
               check (role in ('primary', 'part', 'delta')),
  unique (file_id, part_index)
);

create index if not exists idx_file_objects_file   on storage.file_objects (file_id);
create index if not exists idx_file_objects_object on storage.file_objects (object_id);

-- ============================================================
-- Migration: backfill file_objects from legacy files.object_id
-- ============================================================
-- Idempotent: ON CONFLICT DO NOTHING. Safe to run repeatedly.
do $$
begin
  if exists (
    select 1 from information_schema.columns
    where table_schema = 'storage' and table_name = 'files' and column_name = 'object_id'
  ) then
    insert into storage.file_objects (file_id, object_id, part_index, role)
    select id, object_id, 0, 'primary'
    from storage.files
    where object_id is not null
    on conflict (file_id, part_index) do nothing;
  end if;
end;
$$;

-- Drop the now-redundant 1:1 FK column from storage.files.
-- Only runs if file_objects has been backfilled (all non-null object_ids migrated).
do $$
declare v_missing bigint;
begin
  if exists (
    select 1 from information_schema.columns
    where table_schema = 'storage' and table_name = 'files' and column_name = 'object_id'
  ) then
    select count(*) into v_missing
    from storage.files f
    where f.object_id is not null
      and not exists (
        select 1 from storage.file_objects fo
        where fo.file_id = f.id and fo.part_index = 0
      );
    if v_missing = 0 then
      alter table storage.files drop column if exists object_id;
    else
      raise notice 'storage.files.object_id not dropped: % rows still lack file_objects entries', v_missing;
    end if;
  end if;
end;
$$;

alter table if exists storage.files
add column if not exists full_path text;

alter table if exists storage.files
drop column if exists is_compressed;

alter table if exists storage.files
drop column if exists compression_algorithm;

alter table if exists storage.files
drop column if exists compression_level;

with recursive
  dir_tree as (
    select
      d.id,
      d.parent_id,
      d.name,
      '/' || d.name as path
    from
      storage.directories d
    where
      d.parent_id is null
    union all
    select
      child.id,
      child.parent_id,
      child.name,
      dir_tree.path || '/' || child.name
    from
      storage.directories child
      join dir_tree on child.parent_id = dir_tree.id
  )
update storage.directories d
set
  path = dir_tree.path
from
  dir_tree
where
  d.id = dir_tree.id;

update storage.files f
set
  full_path = case
    when f.directory_id is null then '/' || f.name
    else d.path || '/' || f.name
  end
from
  storage.directories d
where
  f.directory_id = d.id
  and f.full_path is null;

update storage.files
set
  full_path = '/' || name
where
  directory_id is null
  and full_path is null;

alter table if exists storage.files
alter column full_path
set not null;

-- unique indexes (path uniqueness — not object-scoped, those are removed)
create unique index if not exists uq_files_root on storage.files (user_id, name)
where
  directory_id is null
  and user_id is not null
  and domain is null;

create unique index if not exists uq_files_root_service on storage.files (automated_service_id, name)
where
  directory_id is null
  and automated_service_id is not null
  and domain is null;

create unique index if not exists uq_files_directory on storage.files (user_id, directory_id, name)
where
  directory_id is not null
  and user_id is not null
  and domain is null;

create unique index if not exists uq_files_directory_service on storage.files (automated_service_id, directory_id, name)
where
  directory_id is not null
  and automated_service_id is not null
  and domain is null;

-- Legacy object-scoped unique indexes removed (replaced by file_objects).
drop index if exists uq_files_object_root;
drop index if exists uq_files_object_directory;
drop index if exists uq_files_object_root_service;
drop index if exists uq_files_object_directory_service;

-- Partial unique index: each non-null key_id must map to exactly one file
create unique index if not exists uq_files_key_id
    on storage.files (key_id)
    where key_id is not null;

-- ============================================================
-- indexes
-- ============================================================
create index if not exists idx_objects_key on storage.objects (key)
where
  key is not null;

create index if not exists idx_objects_bucket on storage.objects (bucket)
where
  deleted_at is null;

create index if not exists idx_directories_user on storage.directories (user_id);

create index if not exists idx_directories_automated_service on storage.directories (automated_service_id);

create index if not exists idx_directories_session on storage.directories (session_id);

create index if not exists idx_directories_parent on storage.directories (parent_id)
where
  parent_id is not null;

create index if not exists idx_directories_path on storage.directories (path);

create index if not exists idx_files_user on storage.files (user_id);

create index if not exists idx_files_automated_service on storage.files (automated_service_id);

create index if not exists idx_files_session on storage.files (session_id);

create index if not exists idx_files_user_full_path on storage.files (user_id, full_path text_pattern_ops)
where
  user_id is not null;

create index if not exists idx_files_service_full_path on storage.files (automated_service_id, full_path text_pattern_ops)
where
  automated_service_id is not null;

create index if not exists idx_files_directory on storage.files (directory_id)
where
  directory_id is not null;

-- Drop legacy object index (now expressed via file_objects)
drop index if exists idx_files_object;

-- ============================================================
-- view — full path resolution (joins through file_objects)
-- ============================================================
-- For multi-segment files: original_size = SUM of all segment original sizes;
-- compressed_size = SUM of all segment compressed (stored) sizes.
-- Compression metadata is taken from the first segment (part_index=0).
create or replace view storage.file_paths as
select
  f.id,
  f.name,
  f.user_id,
  f.automated_service_id,
  f.session_id,
  f.full_path,
  coalesce(
    nullif(regexp_replace(f.full_path, '/[^/]+$', ''), ''),
    '/'
  ) as directory_path,
  f.directory_id,
  -- Primary object (part_index=0) for key/bucket resolution.
  primary_obj.id   as object_id,
  primary_obj.bucket,
  primary_obj.key  as object_key,
  f.extension,
  primary_obj.is_compressed,
  primary_obj.compression_algorithm,
  primary_obj.compression_level,
  -- Aggregate sizes across all segments
  coalesce(sizes.compressed_size, 0) as compressed_size,
  coalesce(sizes.original_size, 0)   as original_size,
  f.created_at
from
  storage.files f
  -- First segment for metadata
  join storage.file_objects fo0 on fo0.file_id = f.id and fo0.part_index = 0
  join storage.objects primary_obj on primary_obj.id = fo0.object_id
  -- Aggregate across all segments
  join (
    select
      fo.file_id,
      sum(o.size)          as compressed_size,
      sum(o.original_size) as original_size
    from storage.file_objects fo
    join storage.objects o on o.id = fo.object_id
    group by fo.file_id
  ) sizes on sizes.file_id = f.id;

-- ============================================================
-- view — directory tree with recursive size/count data
-- ============================================================
create or replace view storage.directory_tree as
with recursive
  tree as (
    select
      d.id,
      d.parent_id,
      d.user_id,
      d.automated_service_id,
      d.session_id,
      d.name,
      d.path,
      d.virtual_size,
      d.real_size,
      d.count_sub_files,
      d.count_sub_directories,
      d.created_at,
      0 as depth
    from
      storage.directories d
    where
      d.parent_id is null
    union all
    select
      d.id,
      d.parent_id,
      d.user_id,
      d.automated_service_id,
      d.session_id,
      d.name,
      d.path,
      d.virtual_size,
      d.real_size,
      d.count_sub_files,
      d.count_sub_directories,
      d.created_at,
      t.depth + 1 as depth
    from
      storage.directories d
      join tree t on d.parent_id = t.id
  )
select
  *
from
  tree;

-- Recompute directory recursive sizes for existing rows (idempotent backfill).
with recursive
  file_ancestors as (
    select
      f.id as file_id,
      d.id as ancestor_id,
      d.parent_id
    from
      storage.files f
      join storage.directories d on d.id = f.directory_id
    union all
    select
      fa.file_id,
      p.id as ancestor_id,
      p.parent_id
    from
      file_ancestors fa
      join storage.directories p on p.id = fa.parent_id
  ),
  totals as (
    select
      fa.ancestor_id as directory_id,
      coalesce(sum(o.original_size), 0)::bigint as virtual_size,
      coalesce(sum(o.size), 0)::bigint as real_size
    from
      file_ancestors fa
      join storage.files f on f.id = fa.file_id
      join storage.file_objects fo on fo.file_id = f.id
      join storage.objects o on o.id = fo.object_id
    group by
      fa.ancestor_id
  )
update storage.directories d
set
  virtual_size = coalesce(t.virtual_size, 0),
  real_size = coalesce(t.real_size, 0)
from
  (
    select
      d0.id,
      t0.virtual_size,
      t0.real_size
    from
      storage.directories d0
      left join totals t0 on t0.directory_id = d0.id
  ) t
where
  d.id = t.id;

-- ============================================================
-- trigger — keep directories.path materialized
-- ============================================================
create or replace function storage.sync_directory_path () returns trigger language plpgsql as $$
begin
  if new.parent_id is null then
    new.path := '/' || new.name;
  else
    select path || '/' || new.name into new.path
    from storage.directories where id = new.parent_id;
  end if;
  return new;
end;
$$;

create trigger trg_sync_directory_path before insert
or
update of name,
parent_id on storage.directories for each row
execute function storage.sync_directory_path ();

create or replace function storage.refresh_directory_subtree_paths () returns trigger language plpgsql as $$
begin
  with recursive dir_tree as (
    select d.id, d.parent_id, d.name, new.path as path
    from storage.directories d where d.id = new.id
    union all
    select child.id, child.parent_id, child.name, dir_tree.path || '/' || child.name
    from storage.directories child join dir_tree on child.parent_id = dir_tree.id
  )
  update storage.directories d set path = dir_tree.path
  from dir_tree
  where d.id = dir_tree.id and d.path is distinct from dir_tree.path;

  with recursive dir_tree as (
    select d.id, d.parent_id, d.name, new.path as path
    from storage.directories d where d.id = new.id
    union all
    select child.id, child.parent_id, child.name, dir_tree.path || '/' || child.name
    from storage.directories child join dir_tree on child.parent_id = dir_tree.id
  )
  update storage.files f
  set full_path = case
    when f.directory_id is null then '/' || f.name
    else dir_tree.path || '/' || f.name end
  from dir_tree
  where f.directory_id = dir_tree.id
    and f.full_path is distinct from case
      when f.directory_id is null then '/' || f.name
      else dir_tree.path || '/' || f.name end;

  return new;
end;
$$;

create trigger trg_refresh_directory_subtree_paths
after insert
or
update of name,
parent_id on storage.directories for each row
execute function storage.refresh_directory_subtree_paths ();

-- ============================================================
-- trigger — keep files.full_path materialized
-- ============================================================
create or replace function storage.sync_file_path () returns trigger language plpgsql as $$
begin
  if new.directory_id is null then
    new.full_path := '/' || new.name;
  else
    select d.path || '/' || new.name into new.full_path
    from storage.directories d where d.id = new.directory_id;
  end if;
  return new;
end;
$$;

create trigger trg_sync_file_path before insert
or
update of name,
directory_id on storage.files for each row
execute function storage.sync_file_path ();

-- ============================================================
-- function — ensure full directory tree exists (upsert)
-- ============================================================
create or replace function storage.ensure_directory_path (p_user_id int, p_session_id bigint, p_path text) returns bigint language plpgsql as $$
begin
  return storage.ensure_directory_path(p_user_id, null::int, p_session_id, p_path);
end;
$$;

create or replace function storage.ensure_directory_path (
  p_user_id int,
  p_automated_service_id int,
  p_session_id bigint,
  p_path text
) returns bigint language plpgsql as $$
declare
  v_segments text[];
  v_seg      text;
  v_parent_id bigint := null;
  v_dir_id    bigint;
  v_built     text := '';
begin
  v_segments := string_to_array(trim(both '/' from p_path), '/');
  foreach v_seg in array v_segments loop
    v_built := v_built || '/' || v_seg;
    if p_automated_service_id is not null then
      if v_parent_id is null then
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (null, p_automated_service_id, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (automated_service_id, path) where automated_service_id is not null do nothing
        returning id into v_dir_id;
      else
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (null, p_automated_service_id, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (automated_service_id, parent_id, name) where automated_service_id is not null do nothing
        returning id into v_dir_id;
      end if;
    else
      if v_parent_id is null then
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (p_user_id, null, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (user_id, path) where user_id is not null do nothing
        returning id into v_dir_id;
      else
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (p_user_id, null, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (user_id, parent_id, name) where user_id is not null do nothing
        returning id into v_dir_id;
      end if;
    end if;
    if v_dir_id is null then
      if p_automated_service_id is not null then
        select id into strict v_dir_id from storage.directories
        where automated_service_id = p_automated_service_id and path = v_built;
      else
        select id into strict v_dir_id from storage.directories
        where user_id = p_user_id and path = v_built;
      end if;
    end if;
    v_parent_id := v_dir_id;
  end loop;
  return v_dir_id;
end;
$$;

-- ============================================================
-- function — atomic upsert for objects (content-addressable)
-- ============================================================
create or replace function storage.upsert_object (
  p_bucket text,
  p_key text,
  p_size bigint,
  p_original_size bigint,
  p_sha256 char(64),
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
    sha256,
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
    p_sha256,
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
    o.sha256,
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
     or v_existing.sha256 <> p_sha256
     or v_existing.provider is distinct from p_provider
     or v_existing.is_compressed is distinct from p_is_compressed
     or v_existing.compression_algorithm is distinct from p_compression_algorithm
     or v_existing.compression_level is distinct from p_compression_level then
    raise exception 'Conflicting metadata for existing object (bucket=%, key=%)', p_bucket, p_key
      using errcode = '23505';
  end if;

  -- Chunking trace is refresh-metadata, not identity: the same bytes
  -- re-uploaded under a different threshold just update the mode columns.
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

-- ============================================================
-- trigger — maintain count_sub_directories on parent
-- ============================================================
create or replace function storage.trg_update_parent_dir_count () returns trigger language plpgsql as $$
begin
  if tg_op = 'INSERT' then
    if new.parent_id is not null then
      update storage.directories set count_sub_directories = count_sub_directories + 1 where id = new.parent_id;
    end if;
    return new;
  elsif tg_op = 'UPDATE' then
    if old.parent_id is distinct from new.parent_id then
      if old.parent_id is not null then
        update storage.directories set count_sub_directories = count_sub_directories - 1 where id = old.parent_id;
      end if;
      if new.parent_id is not null then
        update storage.directories set count_sub_directories = count_sub_directories + 1 where id = new.parent_id;
      end if;
    end if;
    return new;
  elsif tg_op = 'DELETE' then
    if old.parent_id is not null then
      update storage.directories set count_sub_directories = count_sub_directories - 1 where id = old.parent_id;
    end if;
    return old;
  end if;
  return null;
end;
$$;

create trigger trg_count_sub_directories
after insert
or
update of parent_id
or delete on storage.directories for each row
execute function storage.trg_update_parent_dir_count ();

-- ============================================================
-- trigger — maintain count_sub_files on directory
-- ============================================================
create or replace function storage.trg_update_dir_entry_count () returns trigger language plpgsql as $$
begin
  if tg_op = 'INSERT' then
    if new.directory_id is not null then
      update storage.directories set count_sub_files = count_sub_files + 1 where id = new.directory_id;
    end if;
    return new;
  elsif tg_op = 'UPDATE' then
    if old.directory_id is distinct from new.directory_id then
      if old.directory_id is not null then
        update storage.directories set count_sub_files = count_sub_files - 1 where id = old.directory_id;
      end if;
      if new.directory_id is not null then
        update storage.directories set count_sub_files = count_sub_files + 1 where id = new.directory_id;
      end if;
    end if;
    return new;
  elsif tg_op = 'DELETE' then
    if old.directory_id is not null then
      update storage.directories set count_sub_files = count_sub_files - 1 where id = old.directory_id;
    end if;
    return old;
  end if;
  return null;
end;
$$;

create trigger trg_count_sub_files
after insert
or
update of directory_id
or delete on storage.files for each row
execute function storage.trg_update_dir_entry_count ();

-- ============================================================
-- trigger — maintain recursive virtual/real directory sizes
-- ============================================================
create or replace function storage.apply_directory_size_delta (
  p_directory_id bigint,
  p_virtual_delta bigint,
  p_real_delta bigint
) returns void language plpgsql as $$
begin
  if p_directory_id is null then
    return;
  end if;

  with recursive ancestors as (
    select d.id, d.parent_id
    from storage.directories d
    where d.id = p_directory_id
    union all
    select d.id, d.parent_id
    from storage.directories d
    join ancestors a on d.id = a.parent_id
  )
  update storage.directories d
  set
    virtual_size = greatest(0, coalesce(d.virtual_size, 0) + p_virtual_delta),
    real_size = greatest(0, coalesce(d.real_size, 0) + p_real_delta)
  from ancestors a
  where d.id = a.id;
end;
$$;

create or replace function storage.get_file_sizes (
  p_file_id bigint,
  out virtual_size bigint,
  out real_size bigint
) returns record language plpgsql as $$
begin
  select coalesce(sum(o.original_size), 0), coalesce(sum(o.size), 0)
    into virtual_size, real_size
    from storage.file_objects fo
    join storage.objects o on o.id = fo.object_id
   where fo.file_id = p_file_id;
end;
$$;

create or replace function storage.update_directory_sizes () returns trigger language plpgsql as $$
declare
  v_old_virtual bigint := 0;
  v_old_real bigint := 0;
  v_new_virtual bigint := 0;
  v_new_real bigint := 0;
begin
  if tg_op = 'INSERT' then
    select virtual_size, real_size into v_new_virtual, v_new_real from storage.get_file_sizes(new.id);
    perform storage.apply_directory_size_delta(new.directory_id, v_new_virtual, v_new_real);
    return new;
  elsif tg_op = 'DELETE' then
    select virtual_size, real_size into v_old_virtual, v_old_real from storage.get_file_sizes(old.id);
    perform storage.apply_directory_size_delta(old.directory_id, -v_old_virtual, -v_old_real);
    return old;
  elsif tg_op = 'UPDATE' then
    if old.directory_id is distinct from new.directory_id then
      select virtual_size, real_size into v_old_virtual, v_old_real from storage.get_file_sizes(old.id);
      select virtual_size, real_size into v_new_virtual, v_new_real from storage.get_file_sizes(new.id);

      perform storage.apply_directory_size_delta(old.directory_id, -v_old_virtual, -v_old_real);
      perform storage.apply_directory_size_delta(new.directory_id, v_new_virtual, v_new_real);
    end if;
    return new;
  end if;

  return null;
end;
$$;

drop trigger if exists trg_update_directory_sizes on storage.files;

create trigger trg_update_directory_sizes
after insert
or
update of directory_id
or delete on storage.files for each row
execute function storage.update_directory_sizes ();

-- NOTE:
-- Directory sizes are a decomposition of actor totals (users/services).
-- Organisation totals remain sourced from actor totals via
-- core.sync_org_storage_totals() to avoid double counting.
-- ============================================================
-- trigger — cleanup orphaned objects after file deletion
-- ============================================================
create or replace function storage.cleanup_orphan_objects () returns trigger language plpgsql as $$
begin
  if old.object_id is not null
     and not exists (select 1 from storage.file_objects where object_id = old.object_id) then
    delete from storage.objects where id = old.object_id;
  end if;
  if tg_op = 'UPDATE' then
    return new;
  end if;
  return old;
end;
$$;

drop trigger if exists trg_cleanup_orphan_objects on storage.file_objects;
create trigger trg_cleanup_orphan_objects
after
update of object_id
or delete on storage.file_objects for each row
execute function storage.cleanup_orphan_objects ();

-- ============================================================
-- trigger — maintain total storage usage on users/services
-- ============================================================
-- WP2 (F11): the INSERT branch is disabled.  At BEFORE INSERT on
-- storage.files the file's storage.file_objects mappings do not exist yet,
-- so `storage.get_file_sizes` returned 0 and the old INSERT branch added
-- +1 entry and +0 bytes per file — byte accounting never advanced and
-- byte-based quotas were never enforceable.  Mapping-level accounting,
-- org refresh and byte quota gates now live in
-- storage.apply_object_owner_usage (AFTER INSERT on storage.file_objects).
-- The UPDATE (owner move) and DELETE branches stay: mappings are still
-- readable at BEFORE time on those paths.
-- ============================================================
create or replace function storage.update_owner_storage_usage () returns trigger language plpgsql as $$
declare
  v_old_virtual bigint := 0;
  v_old_real    bigint := 0;
  v_new_virtual bigint := 0;
  v_new_real    bigint := 0;
begin
  if (tg_op = 'INSERT') then
    return new;
  elsif (tg_op = 'DELETE') then
    select virtual_size, real_size into v_old_virtual, v_old_real from storage.get_file_sizes(old.id);

    if old.user_id is not null then
      update core.users
      set total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      where id = old.user_id;
    elsif old.automated_service_id is not null then
      update core.automated_services
      set total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      where id = old.automated_service_id;
    end if;
    return old;
  elsif (tg_op = 'UPDATE') then
    if old.user_id is not distinct from new.user_id
       and old.automated_service_id is not distinct from new.automated_service_id then
      return new;
    end if;

    select virtual_size, real_size into v_old_virtual, v_old_real from storage.get_file_sizes(old.id);
    select virtual_size, real_size into v_new_virtual, v_new_real from storage.get_file_sizes(new.id);

    if old.user_id is not null then
      update core.users
      set total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      where id = old.user_id;
    elsif old.automated_service_id is not null then
      update core.automated_services
      set total_virtual_size = coalesce(total_virtual_size, 0) - v_old_virtual,
          total_real_size    = coalesce(total_real_size, 0)    - v_old_real,
          total_entry_count   = coalesce(total_entry_count, 0) - 1
      where id = old.automated_service_id;
    end if;

    if new.user_id is not null then
      update core.users
      set total_virtual_size = coalesce(total_virtual_size, 0) + v_new_virtual,
          total_real_size    = coalesce(total_real_size, 0)    + v_new_real,
          total_entry_count   = coalesce(total_entry_count, 0) + 1
      where id = new.user_id;
    elsif new.automated_service_id is not null then
      update core.automated_services
      set total_virtual_size = coalesce(total_virtual_size, 0) + v_new_virtual,
          total_real_size    = coalesce(total_real_size, 0)    + v_new_real,
          total_entry_count   = coalesce(total_entry_count, 0) + 1
      where id = new.automated_service_id;
    end if;
    return new;
  end if;

  return null;
end;
$$;

drop trigger if exists trg_update_owner_storage_usage on storage.files;

create trigger trg_update_owner_storage_usage before insert
or
update of user_id,
automated_service_id
or delete on storage.files for each row
execute function storage.update_owner_storage_usage ();

-- ============================================================
-- WP2: actor usage recompute + byte-level quota gates
-- ============================================================
-- Recompute one actor's totals from live file_objects mappings and rows.
-- Used as the single accounting point at mapping-change time so byte totals
-- cannot drift (the old delta path computed +0 bytes per insert — F11).
create or replace function storage.recompute_actor_usage (
  p_user_id int,
  p_service_id int
) returns void language plpgsql as $$
begin
  if p_user_id is not null then
    update core.users
    set total_virtual_size = (
          select coalesce(sum(o.original_size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.user_id = p_user_id
        ),
        total_real_size = (
          select coalesce(sum(o.size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.user_id = p_user_id
        ),
        total_entry_count = (
          select count(*)::bigint from storage.files f where f.user_id = p_user_id
        ) + (
          select count(*)::bigint from storage.directories d where d.user_id = p_user_id
        )
    where id = p_user_id;
  elsif p_service_id is not null then
    update core.automated_services
    set total_virtual_size = (
          select coalesce(sum(o.original_size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.automated_service_id = p_service_id
        ),
        total_real_size = (
          select coalesce(sum(o.size), 0)
          from storage.files f
          join storage.file_objects fo on fo.file_id = f.id
          join storage.objects o on o.id = fo.object_id
          where f.automated_service_id = p_service_id
        ),
        total_entry_count = (
          select count(*)::bigint from storage.files f where f.automated_service_id = p_service_id
        ) + (
          select count(*)::bigint from storage.directories d where d.automated_service_id = p_service_id
        )
    where id = p_service_id;
  end if;
end;
$$;

-- AFTER INSERT / UPDATE OF object_id / DELETE on storage.file_objects:
--   1. Recompute the owning actor's totals from live mappings (actor-less
--      domain rows carry no actor) and refresh the organisation aggregate.
--   2. Enforce the byte caps (actor, organisation, domain) *after* the new
--      mapping state is visible.  Raising here rolls the upload's whole
--      transaction back, files row included.
create or replace function storage.apply_object_owner_usage () returns trigger language plpgsql as $$
declare
  v_file_id     bigint;
  v_user_id     int;
  v_service_id  int;
  v_domain_id   int;
  v_org         text;
  v_usage       bigint;
  v_limit       bigint;
  v_entry_count bigint;
  v_entry_limit bigint;
  v_org_usage   bigint;
  v_org_limit   bigint;
begin
  if tg_op = 'INSERT' or tg_op = 'UPDATE' then
    v_file_id := new.file_id;
  else
    v_file_id := old.file_id;
  end if;

  select f.user_id, f.automated_service_id, f.domain_id
    into v_user_id, v_service_id, v_domain_id
  from storage.files f
  where f.id = v_file_id;

  if not found then
    if tg_op = 'DELETE' then
      return old; -- file already gone: nothing to maintain
    end if;
    raise exception 'file_objects row references missing file %', v_file_id using errcode = '23503';
  end if;

  if v_user_id is not null then
    select organisation into v_org from core.users where id = v_user_id;
  elsif v_service_id is not null then
    select organisation into v_org from core.automated_services where id = v_service_id;
  elsif v_domain_id is not null then
    select organisation into v_org from core.domains where id = v_domain_id;
  end if;

  -- 1. refresh accounting
  if v_user_id is not null or v_service_id is not null then
    perform storage.recompute_actor_usage(v_user_id, v_service_id);
  end if;
  if v_org is not null then
    perform core.sync_org_totals_for(v_org);
  end if;

  -- 2. byte/entry gates (usage now includes the change just made)
  if tg_op <> 'DELETE' then
    -- actor byte + entry caps
    if v_user_id is not null then
      select total_virtual_size, storage_limit, total_entry_count, entry_count_limit
      into v_usage, v_limit, v_entry_count, v_entry_limit
      from core.users where id = v_user_id;

      if v_limit is not null and coalesce(v_usage, 0) > v_limit then
        raise exception 'Actor storage quota exceeded'
          using errcode = '55P03',
                detail  = format('Actor limit: %s, Current: %s', v_limit, coalesce(v_usage, 0));
      end if;
    elsif v_service_id is not null then
      select total_virtual_size, storage_limit, total_entry_count, entry_count_limit
      into v_usage, v_limit, v_entry_count, v_entry_limit
      from core.automated_services where id = v_service_id;

      if v_limit is not null and coalesce(v_usage, 0) > v_limit then
        raise exception 'Actor storage quota exceeded'
          using errcode = '55P03',
                detail  = format('Actor limit: %s, Current: %s', v_limit, coalesce(v_usage, 0));
      end if;
    end if;

    -- organisation byte cap (covers actor-owned and domain-namespace rows)
    if v_org is not null then
      select total_virtual_size, storage_limit
      into v_org_usage, v_org_limit
      from core.organisations where name = v_org;

      if v_org_limit is not null and coalesce(v_org_usage, 0) > v_org_limit then
        raise exception 'Organisation storage quota exceeded'
          using errcode = '55P03',
                detail  = format('Organisation: %s, Org limit: %s, Org current: %s',
                                 v_org, v_org_limit, coalesce(v_org_usage, 0));
      end if;
    end if;

    -- domain byte cap
    if v_domain_id is not null then
      select storage_limit into v_limit from core.domains where id = v_domain_id;

      if v_limit is not null then
        select virtual_size into v_usage from storage.domain_usage(v_domain_id);

        if coalesce(v_usage, 0) > v_limit then
          raise exception 'Domain storage quota exceeded'
            using errcode = '55P03',
                  detail  = format('Domain limit: %s, Current: %s', v_limit, coalesce(v_usage, 0));
        end if;
      end if;
    end if;
  end if;

  if tg_op = 'DELETE' then
    return old;
  end if;
  return new;
end;
$$;

drop trigger if exists trg_apply_object_owner_usage on storage.file_objects;
create trigger trg_apply_object_owner_usage
after insert or update of object_id or delete on storage.file_objects
for each row execute function storage.apply_object_owner_usage ();

-- ============================================================
-- trigger — enforce storage limits on users/services and org
-- ============================================================
-- FIX #9: org-level quota is now checked in addition to the per-actor limit.
-- The check order is: actor limit first, then org limit — so the more
-- specific constraint surfaces first in the error message.
--
-- WP2 (finding F11): the byte-level checks are no longer performed here.
-- This trigger fires BEFORE INSERT on storage.files, but a file's
-- storage.file_objects mappings are inserted *after* the files row, so
-- `v_original_size` aggregated below was always 0 and neither actor nor org
-- byte capacities were ever enforced.  A files-row trigger can never see the
-- bytes of the upload it is vetting; the byte gates therefore moved to
-- storage.apply_object_owner_usage (AFTER INSERT on storage.file_objects),
-- where the mapped object sizes are final.  This function now enforces only
-- the entry-count limits (actor + organisation); domain-namespace entry caps
-- are enforced by storage.enforce_domain_quota.
-- ============================================================
create or replace function storage.enforce_storage_limit () returns trigger language plpgsql as $$
declare
  v_entry_count    bigint;
  v_entry_limit    bigint;
  v_org            text;
  v_org_file_count bigint;
  v_org_file_limit bigint;
begin
  -- 1. per-actor entry check (user or service).  Domain-namespace rows carry
  --    no actor; their caps are enforced in storage.enforce_domain_quota /
  --    storage.apply_object_owner_usage, so bail out when there is none.
  if new.user_id is not null then
    select total_entry_count, entry_count_limit, organisation
    into v_entry_count, v_entry_limit, v_org
    from core.users where id = new.user_id;
  elsif new.automated_service_id is not null then
    select total_entry_count, entry_count_limit, organisation
    into v_entry_count, v_entry_limit, v_org
    from core.automated_services where id = new.automated_service_id;
  end if;

  if v_org is null then
    return new;
  end if;

  if v_entry_limit is not null and (coalesce(v_entry_count, 0) + 1) > v_entry_limit then
    raise exception 'Entry count limit exceeded'
      using errcode = '55P03',
            detail  = format('Actor entry limit: %s, Current count: %s',
                             v_entry_limit, coalesce(v_entry_count, 0));
  end if;

  -- 2. org-level entry limit.  core.organisations totals include
  --    domain-namespace storage (see core.sync_org_totals_for), and the new
  --    row is not yet counted at BEFORE INSERT, hence the +1.
  select total_entry_count, entry_count_limit
  into v_org_file_count, v_org_file_limit
  from core.organisations where name = v_org;

  if v_org_file_limit is not null and (coalesce(v_org_file_count, 0) + 1) > v_org_file_limit then
    raise exception 'Organisation entry count limit exceeded'
      using errcode = '55P03',
            detail  = format('Organisation: %s, Org entry limit: %s, Org current count: %s',
                             v_org, v_org_file_limit, coalesce(v_org_file_count, 0));
  end if;

  return new;
end;
$$;

create trigger trg_enforce_storage_limit before insert on storage.files for each row
execute function storage.enforce_storage_limit ();

create trigger trg_enforce_storage_limit_dir before insert on storage.directories for each row
execute function storage.enforce_storage_limit ();

-- ============================================================
-- WP2: domain-namespace quota support
-- ============================================================

-- Live usage of a domain namespace: byte sums over this domain's
-- file_objects mappings and the entry counts over its rows.  Computed live
-- (not from stored counters) so it cannot drift from the actual mappings.
create or replace function storage.domain_usage (p_domain_id int)
returns table (virtual_size bigint, real_size bigint, entry_count bigint)
language sql stable as $$
  select
    coalesce((
      select sum(o.original_size)
      from storage.files f
      join storage.file_objects fo on fo.file_id = f.id
      join storage.objects o on o.id = fo.object_id
      where f.domain_id = p_domain_id
    ), 0)::bigint,
    coalesce((
      select sum(o.size)
      from storage.files f
      join storage.file_objects fo on fo.file_id = f.id
      join storage.objects o on o.id = fo.object_id
      where f.domain_id = p_domain_id
    ), 0)::bigint,
    (select count(*)::bigint from storage.files f where f.domain_id = p_domain_id)
      + (select count(*)::bigint from storage.directories d where d.domain_id = p_domain_id)
$$;

-- BEFORE INSERT/UPDATE on storage.files and storage.directories:
--   1. Derive `domain_id` for domain-namespace rows from the domain label.
--      The application writes only the label; without this derivation the id
--      column stayed NULL forever and every domain_id-based consumer (quota,
--      rename, admin filters) saw nothing.  The owner's organisation is
--      resolved through the actor row or, for domain-namespace rows
--      (actorless), through the owning core.sessions row.
--   2. Cross-check a pre-set (domain label, domain_id) pair.
--   3. Enforce the domain's entry_count_limit on file rows (live count,
--      directories included), plus the organisation's entry cap for
--      domain-namespace uploads (no actor row exists to carry that check in
--      storage.enforce_storage_limit).
-- Byte caps for the domain are enforced at the mapping stage (see
-- storage.apply_object_owner_usage); at this point a file has no mappings.
create or replace function storage.enforce_domain_quota () returns trigger language plpgsql as $$
declare
  v_org          text;
  v_domain_id    int;
  v_domain_name  text;
  v_entry_limit  bigint;
  v_entry_count  bigint;
  v_org_entry_limit   bigint;
  v_org_entry_count   bigint;
begin
  if new.domain_id is not null then
    select organisation, name into v_org, v_domain_name from core.domains where id = new.domain_id;

    if v_org is null then
      raise exception 'Domain % does not exist', new.domain_id using errcode = '23503';
    end if;

    -- normalise the label from the id; reject mismatched (label, id) pairs
    if new.domain is not null and new.domain is distinct from v_domain_name then
      raise exception 'Domain label % does not match domain id %', new.domain, new.domain_id
        using errcode = '23514';
    end if;
    new.domain := v_domain_name;
  elsif new.domain is not null then
    if new.user_id is not null then
      select organisation into v_org from core.users where id = new.user_id;
    elsif new.automated_service_id is not null then
      select organisation into v_org from core.automated_services where id = new.automated_service_id;
    else
      select coalesce(u.organisation, a.organisation) into v_org
      from core.sessions s
      left join core.users u on u.id = s.user_id
      left join core.automated_services a on a.id = s.automated_service_id
      where s.id = new.session_id;
    end if;

    if v_org is null then
      raise exception 'Domain-scoped row requires a resolvable owner (session %)', new.session_id
        using errcode = '23503';
    end if;

    select d.id into v_domain_id
    from core.domains d
    where d.organisation = v_org and d.name = new.domain;

    if v_domain_id is null then
      raise exception 'Unknown domain % for organisation %', new.domain, v_org
        using errcode = '23503';
    end if;
    new.domain_id := v_domain_id;
  end if;

  if TG_TABLE_NAME = 'files' and new.domain_id is not null then
    -- domain entry cap: live count of rows in this namespace (directories
    -- count as entries too, mirroring the actor-level counting)
    select entry_count_limit into v_entry_limit from core.domains where id = new.domain_id;

    if v_entry_limit is not null then
      v_entry_count := (select count(*) from storage.files f where f.domain_id = new.domain_id)
                     + (select count(*) from storage.directories d where d.domain_id = new.domain_id);

      if v_entry_count + 1 > v_entry_limit then
        raise exception 'Domain entry count limit exceeded'
          using errcode = '55P03',
                detail  = format('Domain %s entry limit: %s, Current entries: %s',
                                 new.domain, v_entry_limit, v_entry_count);
      end if;
    end if;

    -- organisation entry cap for domain-namespace uploads: these rows carry
    -- no actor, so storage.enforce_storage_limit skips them; the org total
    -- (synced by core.sync_org_totals_for, counting domain rows live) does
    -- not include the row being inserted yet, hence the +1 here as well.
    select entry_count_limit into v_org_entry_limit from core.organisations where name = v_org;

    if v_org_entry_limit is not null then
      v_org_entry_count := (
        select total_entry_count from core.organisations where name = v_org
      );

      -- only enforce through the live org view when the org total actually
      -- counts domain rows: actor-owned rows were already gated by
      -- storage.enforce_storage_limit against the same counter.
      if new.user_id is null and new.automated_service_id is null
         and v_org_entry_count + 1 > v_org_entry_limit then
        raise exception 'Organisation entry count limit exceeded'
          using errcode = '55P03',
                detail  = format('Organisation: %s, Org entry limit: %s, Org current count: %s',
                                 v_org, v_org_entry_limit, v_org_entry_count);
      end if;
    end if;
  end if;

  return new;
end;
$$;

drop trigger if exists trg_enforce_domain_quota on storage.files;
create trigger trg_enforce_domain_quota
before insert or update of user_id, automated_service_id, session_id, domain, domain_id
on storage.files
for each row execute function storage.enforce_domain_quota ();

drop trigger if exists trg_enforce_domain_quota_dir on storage.directories;
create trigger trg_enforce_domain_quota_dir
before insert or update of user_id, automated_service_id, session_id, domain, domain_id
on storage.directories
for each row execute function storage.enforce_domain_quota ();

-- ============================================================
-- trigger — maintain total_entry_count on directories
-- ============================================================
create or replace function storage.update_owner_directory_count () returns trigger language plpgsql as $$
begin
  if (tg_op = 'INSERT') then
    if new.user_id is not null then
      update core.users
      set total_entry_count = coalesce(total_entry_count, 0) + 1
      where id = new.user_id;
    elsif new.automated_service_id is not null then
      update core.automated_services
      set total_entry_count = coalesce(total_entry_count, 0) + 1
      where id = new.automated_service_id;
    end if;
    return new;
  elsif (tg_op = 'DELETE') then
    if old.user_id is not null then
      update core.users
      set total_entry_count = coalesce(total_entry_count, 0) - 1
      where id = old.user_id;
    elsif old.automated_service_id is not null then
      update core.automated_services
      set total_entry_count = coalesce(total_entry_count, 0) - 1
      where id = old.automated_service_id;
    end if;
    return old;
  elsif (tg_op = 'UPDATE') then
    if old.user_id is not distinct from new.user_id
       and old.automated_service_id is not distinct from new.automated_service_id then
      return new;
    end if;

    if old.user_id is not null then
      update core.users
      set total_entry_count = coalesce(total_entry_count, 0) - 1
      where id = old.user_id;
    elsif old.automated_service_id is not null then
      update core.automated_services
      set total_entry_count = coalesce(total_entry_count, 0) - 1
      where id = old.automated_service_id;
    end if;

    if new.user_id is not null then
      update core.users
      set total_entry_count = coalesce(total_entry_count, 0) + 1
      where id = new.user_id;
    elsif new.automated_service_id is not null then
      update core.automated_services
      set total_entry_count = coalesce(total_entry_count, 0) + 1
      where id = new.automated_service_id;
    end if;
    return new;
  end if;

  return null;
end;
$$;

drop trigger if exists trg_update_owner_directory_count on storage.directories;

create trigger trg_update_owner_directory_count before insert
or
update of user_id,
automated_service_id
or delete on storage.directories for each row
execute function storage.update_owner_directory_count ();

-- ===========================================================================
-- MIGRATION: Domain Scoping and Domain Permission Systems
-- ===========================================================================

-- 3. Unique indices for domain-scoped directories and files
create unique index if not exists uq_directories_root_domain on storage.directories (domain, path) where domain is not null;
create unique index if not exists uq_directories_child_domain on storage.directories (domain, parent_id, name) where domain is not null;

create unique index if not exists uq_files_root_domain on storage.files (domain, name) where domain is not null and directory_id is null;
create unique index if not exists uq_files_directory_domain on storage.files (domain, directory_id, name) where domain is not null;

-- 4. Overloaded domain-aware ensure_directory_path function
create or replace function storage.ensure_directory_path (
  p_user_id int,
  p_automated_service_id int,
  p_session_id bigint,
  p_path text,
  p_domain text
) returns bigint language plpgsql as $$
declare
  v_segments text[];
  v_seg      text;
  v_parent_id bigint := null;
  v_dir_id    bigint;
  v_built     text := '';
begin
  v_segments := string_to_array(trim(both '/' from p_path), '/');
  foreach v_seg in array v_segments loop
    v_built := v_built || '/' || v_seg;
    if p_domain is not null then
      if v_parent_id is null then
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path, domain)
        values (null, null, p_session_id, v_parent_id, v_seg, v_built, p_domain)
        on conflict (domain, path) where domain is not null do nothing
        returning id into v_dir_id;
      else
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path, domain)
        values (null, null, p_session_id, v_parent_id, v_seg, v_built, p_domain)
        on conflict (domain, parent_id, name) where domain is not null do nothing
        returning id into v_dir_id;
      end if;
    elsif p_automated_service_id is not null then
      if v_parent_id is null then
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (null, p_automated_service_id, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (automated_service_id, path) where automated_service_id is not null do nothing
        returning id into v_dir_id;
      else
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (null, p_automated_service_id, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (automated_service_id, parent_id, name) where automated_service_id is not null do nothing
        returning id into v_dir_id;
      end if;
    else
      if v_parent_id is null then
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (p_user_id, null, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (user_id, path) where user_id is not null do nothing
        returning id into v_dir_id;
      else
        insert into storage.directories (user_id, automated_service_id, session_id, parent_id, name, path)
        values (p_user_id, null, p_session_id, v_parent_id, v_seg, v_built)
        on conflict (user_id, parent_id, name) where user_id is not null do nothing
        returning id into v_dir_id;
      end if;
    end if;
    
    if v_dir_id is null then
      if p_domain is not null then
        select id into strict v_dir_id from storage.directories
        where domain = p_domain and path = v_built;
      elsif p_automated_service_id is not null then
        select id into strict v_dir_id from storage.directories
        where automated_service_id = p_automated_service_id and path = v_built;
      else
        select id into strict v_dir_id from storage.directories
        where user_id = p_user_id and path = v_built;
      end if;
    end if;
    v_parent_id := v_dir_id;
  end loop;
  return v_dir_id;
end;
$$;


-- ============================================================
-- CRUD view exposure
-- ============================================================
-- storage.* tables carry no `organisation` tenant column (ownership is via
-- user_id / automated_service_id / session_id / domain), so they are
-- absent from crud/manifest.json and no CRUD views are generated for them.
-- That is intentional: the hand-authored StorageApiHandler owns file,
-- directory, object, and format access (quota checks, S3 fan-out, scope
-- resolution). If a future storage table gains a direct tenant column,
-- expose it by adding a crud/manifest.json entry.
-- ============================================================
