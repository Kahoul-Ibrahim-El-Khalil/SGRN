create schema if not exists core;

-- ============================================================
-- common functions
-- ============================================================
create or replace function core.trigger_set_timestamp () returns trigger as $$
begin
  new.updated_at = now();
  return new;
end;
$$ language plpgsql;

-- ============================================================
-- tables
-- ============================================================
-- FIX #9: Added total_virtual_size, total_real_size, storage_limit to
-- organisations so a global cap per customer can be expressed and enforced.
-- These are kept in sync by core.sync_org_storage_totals() below.
create table if not exists core.organisations (
  id int generated always as identity primary key,
  name text unique not null check (name !~ '[.@/]'),
  description text,
  -- network / web domain for the organisation (free-form, not an FK to core.domains)
  domain text,
  status text not null default 'active' check (status in ('active', 'suspended', 'archived')),
  total_virtual_size bigint not null default 0,
  total_real_size bigint not null default 0,
  storage_limit bigint default null, -- null = unlimited
  total_entry_count bigint not null default 0,
  entry_count_limit bigint default null,
  rate_limit_rpm int default null -- null = default system RPM
);

-- FIX #5: core.domains now holds the canonical operational-domain names
-- (production, maintenance, …). Domain labels are lower-case by contract;
-- dotted canonical authorities are rendered from the slash-separated path.
-- The unique (organisation, name) constraint
-- is the target for composite FKs on users and automated_services,
-- making dangling domain text values impossible.
create table if not exists core.domains (
  id int generated always as identity primary key,
  organisation text not null references core.organisations (name) on delete cascade on update cascade,
  name text not null,
  constraint chk_domains_name check (name !~ '[.@/]'),
  constraint chk_domains_name_lower check (name = lower(name)),
  parent_id int references core.domains (id) on delete restrict default null,
  path text not null default '',
  -- Encryption policy for files stored in this domain.
  -- 'none'     – plaintext only (default)
  -- 'optional' – client may request encryption at upload time
  -- 'required' – server rejects plaintext uploads; all new files must be encrypted
  encryption_policy text not null default 'none'
      check (encryption_policy in ('none', 'optional', 'required')),
  -- WP2: per-domain capacity caps (NULL = unlimited).  Enforced by
  -- storage.enforce_domain_quota (entry counts) and the file_objects byte
  -- gate (storage.apply_object_owner_usage); overrides reported via
  -- storage.domain_usage().  Never client-writable: locked in
  -- crud/manifest.json and changed only through hand-written admin routes.
  storage_limit bigint default null,
  entry_count_limit bigint default null,
  unique (organisation, name),
  unique (organisation, path)
);

create table if not exists core.automated_services (
  id serial primary key,
  -- Organisation ownership
  organisation text not null references core.organisations (name) on delete cascade on update cascade,
  -- Identity & Metadata
  name text not null,
  description text,
  -- Auth
  token uuid not null unique,
  token_secret_hash text not null,
  domain text default null,
  constraint chk_automated_service_domain_lower check (domain is null or domain = lower(domain)),
  -- FIX #7: soft-delete — set deleted_at instead of hard-deleting.
  -- Views and queries filter on deleted_at IS NULL.
  deleted_at timestamptz default null,
  status text not null default 'active' check (
    status in ('active', 'degraded', 'offline', 'maintenance')
  ),
  metadata jsonb not null default '{}',
  total_virtual_size bigint not null default 0,
  total_real_size bigint not null default 0,
  storage_limit bigint default null,
  total_entry_count bigint not null default 0,
  entry_count_limit bigint default 10000,
  rate_limit_rpm int default null,
  max_file_size_mb bigint default null,
  preferred_chunk_size_mb int default null,
  rate_limit_upload_rpm int default null,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  -- FIX #5: composite FK — domain must exist in core.domains for this organisation
  constraint fk_automated_services_domain foreign key (organisation, domain) references core.domains (organisation, name) on update cascade on delete set null
);

create table if not exists core.users (
  id int generated always as identity primary key,
  organisation text not null references core.organisations (name) on delete cascade on update cascade,
  first_name varchar(64) not null,
  family_name varchar(64) not null,
  email varchar(128) not null unique,
  password text not null,
  phone_number varchar(15) unique check (phone_number ~ '^\+?[0-9]{7,15}$'),
  is_platform_admin boolean not null default false,
  -- DEPRECATED: Retained for backward compatibility. Authoritative access control is resolved via core.admin_assignments and core.admin_level().
  role text not null default 'user' check (role in ('admin', 'user')),
  -- FIX #5: domain is now a composite FK to core.domains(organisation, name).
  domain text default null,
  -- FIX #7: soft-delete column.
  deleted_at timestamptz default null,
  status text not null default 'active' check (status in ('active', 'suspended', 'invited')),
  is_active boolean not null default true,
  can_read_personal boolean not null default true,
  can_write_personal boolean not null default true,
  can_delete_personal boolean not null default true,
  total_virtual_size bigint not null default 0,
  total_real_size bigint not null default 0,
  storage_limit bigint default null,
  total_entry_count bigint not null default 0,
  entry_count_limit bigint default 10000,
  rate_limit_rpm int default null,
  max_file_size_mb bigint default null,
  preferred_chunk_size_mb int default null,
  rate_limit_upload_rpm int default null,
  created_at timestamptz not null default now(),
  -- FIX #5: composite FK — domain must exist in core.domains for this organisation
  constraint chk_users_domain_lower check (domain is null or domain = lower(domain)),
  constraint fk_users_domain foreign key (organisation, domain) references core.domains (organisation, name) on update cascade on delete set null
);

create table if not exists core.user_domain_permissions (
  id bigserial primary key,
  user_id int not null references core.users (id) on delete cascade,
  organisation text not null,
  domain text not null,
  allowed_subpath varchar(1024) default '/' not null,
  can_read boolean not null default true,
  can_write boolean not null default true,
  can_delete boolean not null default false,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  constraint fk_user_domain_permission_org_domain foreign key (organisation, domain) references core.domains (organisation, name) on update cascade on delete cascade,
  constraint uq_user_organisation_domain unique (user_id, organisation, domain)
);

create table if not exists core.admin_assignments (
  id bigserial primary key,
  user_id int not null references core.users(id) on delete cascade,
  scope text not null check (scope in ('organisation', 'domain')),
  organisation text not null references core.organisations(name) on delete cascade on update cascade,
  domain_id int references core.domains(id) on delete cascade,
  can_manage_users boolean not null default true,
  can_manage_keys boolean not null default true,
  granted_by int references core.users(id) on delete set null,
  created_at timestamptz not null default now(),
  constraint chk_admin_assignment_scope check (
    (scope = 'organisation' and domain_id is null) or
    (scope = 'domain' and domain_id is not null)
  )
);

create unique index if not exists uq_admin_assignment
  on core.admin_assignments (user_id, scope, organisation, coalesce(domain_id, 0));

create index if not exists idx_admin_assignments_user on core.admin_assignments (user_id);
create index if not exists idx_admin_assignments_org on core.admin_assignments (organisation);
create index if not exists idx_admin_assignments_domain on core.admin_assignments (domain_id) where domain_id is not null;

-- ============================================================
-- WP2: tenant consistency guard for admin assignments
-- ============================================================
-- core.admin_assignments.domain_id carries a single-column FK; without this
-- guard an administrator of organisation A could hold a "domain admin" row
-- that points into organisation B's tree (a cross-tenant escalation route:
-- core.admin_level would then resolve the attacker's level against
-- assignment.organisation while the referenced domain lives elsewhere).
-- The user_id check (both tables) also rejects assignment/permission rows
-- naming a user of a different organisation than the row's own.
create or replace function core.trg_assert_same_org () returns trigger language plpgsql as
$$
declare
  v_domain_org text;
begin
  if not exists (
    select 1 from core.users u
    where u.id = new.user_id and u.organisation = new.organisation
  ) then
    raise exception 'User % does not belong to organisation %', new.user_id, new.organisation
      using errcode = '23514';
  end if;

  if tg_table_name = 'admin_assignments' and new.domain_id is not null then
    select organisation into v_domain_org from core.domains where id = new.domain_id;

    if not found then
      raise exception 'Admin assignment references missing domain %', new.domain_id
        using errcode = '23503';
    end if;

    if v_domain_org is distinct from new.organisation then
      raise exception
        'Cross-organisation admin assignment rejected: domain % belongs to %, assignment targets %',
        new.domain_id, v_domain_org, new.organisation
        using errcode = '23514';
    end if;
  end if;

  return new;
end;
$$;

drop trigger if exists trg_assert_same_org on core.admin_assignments;
create trigger trg_assert_same_org
before insert or update of user_id, organisation, domain_id on core.admin_assignments
for each row execute function core.trg_assert_same_org ();

drop trigger if exists trg_assert_same_org on core.user_domain_permissions;
create trigger trg_assert_same_org
before insert or update of user_id, organisation on core.user_domain_permissions
for each row execute function core.trg_assert_same_org ();

-- ============================================================
-- Domain path trigger & hierarchy functions
-- ============================================================
create or replace function core.trg_maintain_domain_path() returns trigger language plpgsql as $$
declare
  v_parent_org text;
  v_parent_path text;
begin
  if new.parent_id is null then
    new.path := '/' || new.name;
  else
    select organisation, path into v_parent_org, v_parent_path
    from core.domains where id = new.parent_id;

    if not found then
      raise exception 'Parent domain id % does not exist', new.parent_id;
    end if;

    if v_parent_org != new.organisation then
      raise exception 'Parent domain % belongs to organisation %, expected %',
        new.parent_id, v_parent_org, new.organisation;
    end if;

    new.path := v_parent_path || '/' || new.name;
  end if;

  return new;
end;
$$;

drop trigger if exists trg_domain_path on core.domains;
create trigger trg_domain_path
before insert or update of name, parent_id, organisation on core.domains
for each row execute function core.trg_maintain_domain_path();

create or replace function core.move_domain_subtree(
  p_domain_id int,
  p_new_parent_id int
) returns void language plpgsql as $$
declare
  v_org text;
  v_name text;
  v_old_path text;
  v_new_path text;
  v_parent_org text;
  v_parent_path text;
begin
  select organisation, name, path into v_org, v_name, v_old_path
  from core.domains where id = p_domain_id;

  if not found then
    raise exception 'Domain % does not exist', p_domain_id;
  end if;

  if p_new_parent_id is not null then
    if p_new_parent_id = p_domain_id then
      raise exception 'Domain cannot be parent of itself';
    end if;

    select organisation, path into v_parent_org, v_parent_path
    from core.domains where id = p_new_parent_id;

    if not found then
      raise exception 'Target parent domain % does not exist', p_new_parent_id;
    end if;

    if v_parent_org != v_org then
      raise exception 'Target parent domain belongs to different organisation';
    end if;

    if v_parent_path = v_old_path or left(v_parent_path, length(v_old_path) + 1) = v_old_path || '/' then
      raise exception 'Cannot move domain into its own descendant subtree';
    end if;

    v_new_path := v_parent_path || '/' || v_name;
  else
    v_new_path := '/' || v_name;
  end if;

  update core.domains
  set parent_id = p_new_parent_id,
      path = v_new_path
  where id = p_domain_id;

  update core.domains
  set path = v_new_path || substring(path from length(v_old_path) + 1)
  where organisation = v_org
    and left(path, length(v_old_path) + 1) = v_old_path || '/';
end;
$$;

-- ============================================================
-- WP2: rename_domain — the only supported way to change a domain label
-- ============================================================
-- Generated CRUD can no longer update `name`/`parent_id` (locked in
-- crud/manifest.json) because a plain rename silently desynchronises the
-- storage namespace text columns (storage.files/directories/upload_sessions
-- carry `domain` text without an FK).  This function performs the rename
-- atomically: trg_maintain_domain_path recomputes core.domains.path, the
-- composite FKs cascade the rename into users/automated_services/
-- user_domain_permissions, and the storage namespace text columns are
-- rewritten here against domain_id.
create or replace function core.rename_domain (
  p_domain_id int,
  p_new_name text
) returns void language plpgsql as $$
declare
  v_old_name text;
  v_org text;
  v_old_path text;
  v_new_path text;
begin
  if p_new_name is null or p_new_name = '' then
    raise exception 'Domain name must not be empty' using errcode = '23514';
  end if;

  if p_new_name <> lower(p_new_name) then
    raise exception 'Domain names are lower-case' using errcode = '23514';
  end if;

  if p_new_name ~ '[.@/]' then
    raise exception 'Domain names must not contain . @ or /' using errcode = '23514';
  end if;

  select name, organisation, path into v_old_name, v_org, v_old_path
  from core.domains where id = p_domain_id for update;

  if not found then
    raise exception 'Domain % does not exist', p_domain_id using errcode = '23503';
  end if;

  if v_old_name = p_new_name then
    return;
  end if;

  if exists (
    select 1 from core.domains d
    where d.organisation = v_org and d.name = p_new_name and d.id <> p_domain_id
  ) then
    raise exception 'Domain label % already exists in organisation %', p_new_name, v_org
      using errcode = '23505';
  end if;

  -- trg_domain_path rewrites this row's own path from its (unchanged) parent.
  v_new_path := left(v_old_path, length(v_old_path) - length(v_old_name)) || p_new_name;

  -- Composite FKs on (organisation, domain) cascade the rename into
  -- users/automated_services/user_domain_permissions.
  update core.domains set name = p_new_name where id = p_domain_id;

  -- Descendants keep their own labels, but their paths embed the old prefix.
  update core.domains
     set path = v_new_path || substring(path from length(v_old_path) + 1)
   where organisation = v_org
     and left(path, length(v_old_path) + 1) = v_old_path || '/';

  update storage.directories set domain = p_new_name where domain_id = p_domain_id;
  update storage.files set domain = p_new_name where domain_id = p_domain_id;
  update storage.upload_sessions set domain = p_new_name where domain_id = p_domain_id;
end;
$$;

-- ============================================================
-- WP2: create_organisation — validated single-entry creation path
-- ============================================================
-- Organisations are intentionally absent from crud/manifest.json (no tenant
-- column on the row itself); hand-written admin routes create them through
-- this function so the same char/edge validation runs everywhere.
create or replace function core.create_organisation (
  p_name text,
  p_description text default null,
  p_domain text default null,
  p_storage_limit bigint default null,
  p_entry_count_limit bigint default null,
  p_admin_email text default null,
  p_admin_first_name text default null,
  p_admin_family_name text default null,
  p_admin_password text default null,
  p_granted_by int default null
) returns table (out_organisation text, out_admin_user_id int)
language plpgsql as $$
declare
  v_admin_id int;
begin
  if p_name is null then
    raise exception 'Organisation name must not be null' using errcode = '23514';
  end if;

  p_name := trim(p_name);
  if p_name = '' then
    raise exception 'Organisation name must not be empty' using errcode = '23514';
  end if;

  if p_name ~ '[.@/]' then
    raise exception 'Organisation names must not contain . @ or /' using errcode = '23514';
  end if;

  if p_storage_limit is not null and p_storage_limit < 0 then
    raise exception 'storage_limit must be null or non-negative' using errcode = '23514';
  end if;

  if p_entry_count_limit is not null and p_entry_count_limit < 0 then
    raise exception 'entry_count_limit must be null or non-negative' using errcode = '23514';
  end if;

  -- Atomic: the organisation and its optional first administrator land in the
  -- same transaction, so a duplicate name/email rolls the whole thing back and
  -- never leaves an orphan user behind.
  insert into core.organisations (name, description, domain, storage_limit, entry_count_limit)
  values (p_name, p_description, nullif(trim(p_domain), ''), p_storage_limit, p_entry_count_limit);

  if p_admin_email is not null then
    if coalesce(trim(p_admin_first_name), '') = '' or coalesce(trim(p_admin_family_name), '') = '' then
      raise exception 'Initial admin first and family name are required' using errcode = '23514';
    end if;

    if p_admin_password is null or length(p_admin_password) < 12 then
      raise exception 'Initial admin password must be at least 12 characters' using errcode = '23514';
    end if;

    -- trg_hash_password hashes the plaintext on insert.
    insert into core.users (organisation, first_name, family_name, email, password, role)
    values (p_name, trim(p_admin_first_name), trim(p_admin_family_name), lower(trim(p_admin_email)), p_admin_password, 'admin')
    returning id into v_admin_id;

    insert into core.admin_assignments (user_id, scope, organisation, granted_by)
    values (v_admin_id, 'organisation', p_name, p_granted_by);
  end if;

  return query select p_name, v_admin_id;
end;
$$;

-- ============================================================
-- Three-level administrative authorization resolver:
--   3 = Platform Administrator
--   2 = Organisation Administrator
--   1 = Domain (or Domain Ancestor) Administrator
--   0 = None
-- ============================================================
create or replace function core.admin_level(
  p_user_id int,
  p_org text,
  p_domain_id int default null
) returns int language plpgsql stable as $$
declare
  v_is_platform boolean;
  v_user_deleted timestamptz;
  v_target_path text;
  v_target_org text;
begin
  -- 1. Check user exists, not soft-deleted, and platform admin status
  select is_platform_admin, deleted_at into v_is_platform, v_user_deleted
  from core.users where id = p_user_id;

  if not found or v_user_deleted is not null then
    return 0;
  end if;

  if v_is_platform then
    return 3;
  end if;

  if p_org is null then
    return 0;
  end if;

  -- 2. Check organisation admin assignment
  if exists (
    select 1 from core.admin_assignments
    where user_id = p_user_id
      and scope = 'organisation'
      and organisation = p_org
  ) then
    return 2;
  end if;

  -- 3. Check domain admin assignment (if p_domain_id provided)
  if p_domain_id is not null then
    select organisation, path into v_target_org, v_target_path
    from core.domains where id = p_domain_id;

    if not found or v_target_org != p_org then
      return 0;
    end if;

    if exists (
      select 1
      from core.admin_assignments a
      join core.domains d on d.id = a.domain_id
      where a.user_id = p_user_id
        and a.scope = 'domain'
        and a.organisation = p_org
        and (
          d.path = v_target_path
          or (
            length(v_target_path) > length(d.path)
            and left(v_target_path, length(d.path) + 1) = d.path || '/'
          )
        )
    ) then
      return 1;
    end if;
  end if;

  return 0;
end;
$$;

create index if not exists idx_user_domain_perms on core.user_domain_permissions (user_id, organisation, domain);

create index if not exists idx_users_email on core.users (email);

create index if not exists idx_users_role on core.users (role);

create index if not exists idx_users_organisation on core.users (organisation);

create index if not exists idx_users_status on core.users (status);

-- FIX #7: partial index for fast "active users" lookups
create index if not exists idx_users_active on core.users (organisation)
where
  deleted_at is null;

-- ============================================================
-- password hashing trigger
-- ============================================================
create or replace function core.hash_password () returns trigger language plpgsql as $$
begin
  if new.password is not null
    and left(new.password, 4) != '$2a$'
    and left(new.password, 4) != '$2b$' then
    new.password := crypt(new.password, gen_salt('bf'));
  end if;
  return new;
end;
$$;

create trigger trg_hash_password before insert
or
update of password on core.users for each row
execute function core.hash_password ();

-- ============================================================
-- automated_services triggers
-- ============================================================
create trigger trg_set_timestamp_automated_services before
update on core.automated_services for each row
execute function core.trigger_set_timestamp ();

create unique index if not exists idx_automated_services_token on core.automated_services (token);

create index if not exists idx_automated_services_organisation on core.automated_services (organisation);

-- FIX #7: partial index for fast "active services" lookups
create index if not exists idx_automated_services_active on core.automated_services (organisation)
where
  deleted_at is null;

create or replace function core.hash_automated_service_secret () returns trigger language plpgsql as $$
begin
  if new.token_secret_hash is not null
    and left(new.token_secret_hash, 4) != '$2a$'
    and left(new.token_secret_hash, 4) != '$2b$' then
    new.token_secret_hash := crypt(new.token_secret_hash, gen_salt('bf'));
  end if;
  return new;
end;
$$;

create trigger trg_hash_automated_service_secret before insert
or
update of token_secret_hash on core.automated_services for each row
execute function core.hash_automated_service_secret ();

-- ============================================================
-- FIX #9: org-level storage quota sync
-- ============================================================
-- Fires after any change to total_virtual_size / total_real_size (or a
-- soft-delete, which removes a user/service from the live total) on either
-- core.users or core.automated_services, and recomputes the organisation's
-- aggregate so that core.organisations always reflects live totals.
--
-- Using a recompute-on-change approach (vs. delta arithmetic) ensures
-- correctness even if rows are bulk-updated outside normal write paths.
--
-- WP2: the recompute lives in core.sync_org_totals_for(p_org) so that the
-- file_objects usage trigger can refresh an organisation after domain-scoped
-- mapping inserts too (those carry no actor row).  The aggregate now also
-- counts domain-namespace storage, which has no owner row and was previously
-- invisible at the org level.
-- ============================================================
create or replace function core.sync_org_totals_for (p_org text) returns void language plpgsql as $$
begin
  update core.organisations
  set
    total_virtual_size = (
      select coalesce(sum(u.total_virtual_size), 0)
      from core.users u
      where u.organisation = p_org and u.deleted_at is null
    ) + (
      select coalesce(sum(a.total_virtual_size), 0)
      from core.automated_services a
      where a.organisation = p_org and a.deleted_at is null
    ) + (
      -- domain-namespace storage: no actor owns these rows
      select coalesce(sum(o.original_size), 0)
      from storage.files f
      join storage.file_objects fo on fo.file_id = f.id
      join storage.objects o on o.id = fo.object_id
      where f.user_id is null
        and f.automated_service_id is null
        and f.domain_id in (select d.id from core.domains d where d.organisation = p_org)
    ),
    total_real_size = (
      select coalesce(sum(u.total_real_size), 0)
      from core.users u
      where u.organisation = p_org and u.deleted_at is null
    ) + (
      select coalesce(sum(a.total_real_size), 0)
      from core.automated_services a
      where a.organisation = p_org and a.deleted_at is null
    ) + (
      select coalesce(sum(o.size), 0)
      from storage.files f
      join storage.file_objects fo on fo.file_id = f.id
      join storage.objects o on o.id = fo.object_id
      where f.user_id is null
        and f.automated_service_id is null
        and f.domain_id in (select d.id from core.domains d where d.organisation = p_org)
    ),
    total_entry_count = (
      select coalesce(sum(u.total_entry_count), 0)
      from core.users u
      where u.organisation = p_org and u.deleted_at is null
    ) + (
      select coalesce(sum(a.total_entry_count), 0)
      from core.automated_services a
      where a.organisation = p_org and a.deleted_at is null
    ) + (
      select count(*) from storage.files f
      where f.user_id is null
        and f.automated_service_id is null
        and f.domain_id in (select d.id from core.domains d where d.organisation = p_org)
    ) + (
      select count(*) from storage.directories d
      where d.user_id is null
        and d.automated_service_id is null
        and d.domain_id in (select d2.id from core.domains d2 where d2.organisation = p_org)
    )
  where name = p_org;
end;
$$;

create or replace function core.sync_org_storage_totals () returns trigger language plpgsql as $$
declare
  v_org text;
begin
  v_org := coalesce(new.organisation, old.organisation);

  perform core.sync_org_totals_for(v_org);

  if tg_op = 'DELETE' then
    return old;
  end if;
  return new;
end;
$$;

create trigger trg_sync_org_storage_users
after insert
or
update of total_virtual_size,
total_real_size,
total_entry_count,
deleted_at
or delete on core.users for each row
execute function core.sync_org_storage_totals ();

create trigger trg_sync_org_storage_services
after insert
or
update of total_virtual_size,
total_real_size,
total_entry_count,
deleted_at
or delete on core.automated_services for each row
execute function core.sync_org_storage_totals ();

-- ============================================================
-- sessions  (shared by users and automated_services)
-- ============================================================
create table if not exists core.sessions (
  id bigserial primary key,
  user_id int references core.users (id) on delete cascade,
  automated_service_id int references core.automated_services (id) on delete cascade,
  constraint session_owner_check check (
    (
      user_id is not null
      and automated_service_id is null
    )
    or (
      user_id is null
      and automated_service_id is not null
    )
  ),
  token uuid not null,
  ip inet not null,
  session_data jsonb default null,
  expires_at timestamptz default null,
  created_at timestamptz not null default now(),
  terminated_at timestamptz default null,
  termination_reason text default null check (
    termination_reason in (
      'logout',
      'expired',
      'revoked',
      'password_changed',
      'reconnected'
    )
  ),
  constraint chk_termination_consistent check (
    (
      terminated_at is null
      and termination_reason is null
    )
    or (
      terminated_at is not null
      and termination_reason is not null
    )
  )
);

create index if not exists idx_sessions_user_id on core.sessions (user_id)
where
  user_id is not null;

create index if not exists idx_sessions_automated_service_id on core.sessions (automated_service_id)
where
  automated_service_id is not null;

create index if not exists idx_sessions_token on core.sessions (token);

create index if not exists idx_sessions_active on core.sessions (created_at)
where
  terminated_at is null;

-- ============================================================
-- CRUD view exposure (consumed by generate_views.py via crud/manifest.json)
-- ============================================================
-- Which tables/operations/filters/columns the API exposes is declared in
-- crud/manifest.json — not here. Manifest defaults: filter_ops = 0
-- (closed, not searchable), insertable/updatable = true (open, writable).
-- The tenant column (`organisation`) is auto-detected and always bound
-- server-side, never client-settable. Tables absent from the manifest are
-- not generated (write a hand-authored IHandler for them instead).
-- ============================================================

-- organisations (no tenant column => absent from crud/manifest.json;
-- served by hand-written handlers, never raw CRUD)
-- (filter intent for its columns, if ever needed, belongs in the manifest)

-- users (filter/write flags live in crud/manifest.json)
-- password hash is never exposed via CRUD: the generator omits it from the
-- field list entirely (neither readable, filterable, nor writable).

-- automated_services (filter/write flags live in crud/manifest.json)
-- token is a public identifier: readable, but never client-settable.

-- Idempotent column additions for existing databases
alter table core.users add column if not exists max_file_size_mb bigint default null;
alter table core.users add column if not exists preferred_chunk_size_mb int default null;
alter table core.users add column if not exists rate_limit_upload_rpm int default null;

alter table core.automated_services add column if not exists max_file_size_mb bigint default null;
alter table core.automated_services add column if not exists preferred_chunk_size_mb int default null;
alter table core.automated_services add column if not exists rate_limit_upload_rpm int default null;

-- Encryption policy on domains (idempotent for existing databases)
alter table core.domains add column if not exists encryption_policy text not null default 'none'
    check (encryption_policy in ('none', 'optional', 'required'));

-- WP2: per-domain capacity caps (idempotent for existing databases)
alter table core.domains add column if not exists storage_limit bigint default null;
alter table core.domains add column if not exists entry_count_limit bigint default null;

do $$
begin
  if not exists (select 1 from pg_constraint where conname = 'chk_domains_storage_limit_nonneg') then
    alter table core.domains add constraint chk_domains_storage_limit_nonneg
      check (storage_limit is null or storage_limit >= 0);
  end if;

  if not exists (select 1 from pg_constraint where conname = 'chk_domains_entry_count_limit_nonneg') then
    alter table core.domains add constraint chk_domains_entry_count_limit_nonneg
      check (entry_count_limit is null or entry_count_limit >= 0);
  end if;
end $$;

create or replace function core.domain_canonical(p_domain_id int) returns text
language sql stable as $$
    select '@' || d.organisation || '.' || replace(ltrim(d.path, '/'), '/', '.')
    from core.domains d where d.id = p_domain_id
$$;

-- secret hash is never exposed via CRUD: omitted from the field list entirely.

-- user_domain_permissions (filter/write flags live in crud/manifest.json)

-- sessions: never exposed as raw CRUD (tokens + IPs); the auth service owns it.

-- ============================================================
-- session_notify_queue  (cross-instance cache invalidation)
-- ============================================================
-- SessionStore instances poll this table every 500ms and delete processed rows.
-- Rows are inserted automatically by the trigger below, or manually via
-- SessionStore::pgNotify() (which calls pg_notify AND inserts here).
-- This hybrid approach works with Drogon's connection pool (no raw PQnotifies).
create table if not exists core.session_notify_queue (
  id         bigserial primary key,
  action     text not null check (action in ('revoke', 'refresh')),
  token      text not null,
  created_at timestamptz not null default now()
);

create index if not exists idx_session_notify_queue_created
  on core.session_notify_queue (created_at);

-- Auto-clean rows older than 5 minutes (processed by all instances by then).
-- Runs inline on each poll DELETE to avoid a separate maintenance job.

-- Trigger: auto-enqueue a 'revoke' row whenever a session is terminated.
-- This fires even for direct SQL updates (e.g. admin force-logout, expired
-- cleanup jobs) that bypass the application's SessionStore::revokeSession path.
create or replace function core.enqueue_session_revoke() returns trigger language plpgsql as $$
begin
  if new.terminated_at is not null and (old.terminated_at is null) then
    insert into core.session_notify_queue (action, token)
    values ('revoke', new.token::text);
    perform pg_notify('sgrn_session_events',
      json_build_object('action', 'revoke', 'token', new.token::text)::text);
  end if;
  return new;
end;
$$;

create trigger trg_enqueue_session_revoke
after update of terminated_at on core.sessions
for each row execute function core.enqueue_session_revoke();

-- Cleanup old processed notifications (idempotent, safe to run repeatedly)
create or replace function core.purge_old_session_notifications() returns void language sql as $$
  delete from core.session_notify_queue where created_at < now() - interval '5 minutes';
$$;

-- ============================================================
-- WP2: lifecycle-driven session revocation
-- ============================================================
-- Suspending / soft-deleting an account (or suspending / archiving a whole
-- organisation) previously left every issued session token live until its
-- natural expiry — only the *sign-in* path was gated.  These triggers
-- terminate live sessions at the database layer; each terminated row fires
-- trg_enqueue_session_revoke, so every instance's notify loop invalidates
-- its cached token too.
--
-- Reactivation does not resurrect sessions: tokens must be re-issued by a
-- fresh sign-in.

create or replace function core.trg_revoke_user_sessions () returns trigger language plpgsql as $$
begin
  if new.status is distinct from 'active'
     or new.is_active = false
     or new.deleted_at is not null then
    update core.sessions
    set terminated_at = now(),
        termination_reason = 'revoked'
    where user_id = new.id
      and terminated_at is null;
  end if;
  return new;
end;
$$;

drop trigger if exists trg_revoke_user_sessions on core.users;
create trigger trg_revoke_user_sessions
after update of status, is_active, deleted_at on core.users
for each row execute function core.trg_revoke_user_sessions ();

create or replace function core.trg_revoke_service_sessions () returns trigger language plpgsql as $$
begin
  if new.deleted_at is not null then
    update core.sessions
    set terminated_at = now(),
        termination_reason = 'revoked'
    where automated_service_id = new.id
      and terminated_at is null;
  end if;
  return new;
end;
$$;

drop trigger if exists trg_revoke_service_sessions on core.automated_services;
create trigger trg_revoke_service_sessions
after update of status, deleted_at on core.automated_services
for each row execute function core.trg_revoke_service_sessions ();

create or replace function core.trg_revoke_org_sessions () returns trigger language plpgsql as $$
begin
  if new.status is distinct from 'active' then
    update core.sessions s
    set terminated_at = now(),
        termination_reason = 'revoked'
    where s.terminated_at is null
      and (
        s.user_id in (select u.id from core.users u where u.organisation = new.name)
        or s.automated_service_id in (select a.id from core.automated_services a where a.organisation = new.name)
      );
  end if;
  return new;
end;
$$;

drop trigger if exists trg_revoke_org_sessions on core.organisations;
create trigger trg_revoke_org_sessions
after update of status on core.organisations
for each row execute function core.trg_revoke_org_sessions ();

