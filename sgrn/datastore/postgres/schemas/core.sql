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
  name text unique not null,
  description text,
  -- network / web domain for the organisation (free-form, not an FK to core.domains)
  domain text,
  status text not null default 'active' check (status in ('active', 'suspended', 'archived')),
  total_virtual_size bigint not null default 0,
  total_real_size bigint not null default 0,
  storage_limit bigint default null, -- null = unlimited
  total_entry_count bigint not null default 0,
  entry_count_limit bigint default null
);

-- FIX #5: core.domains now holds the canonical operational-domain names
-- (Production, Maintenance, …). The unique (organisation, name) constraint
-- is the target for composite FKs on users and automated_services,
-- making dangling domain text values impossible.
create table if not exists core.domains (
  id int generated always as identity primary key,
  organisation text not null references core.organisations (name) on delete cascade on update cascade,
  name text not null,
  unique (organisation, name)
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
  created_at timestamptz not null default now(),
  -- FIX #5: composite FK — domain must exist in core.domains for this organisation
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
-- ============================================================
create or replace function core.sync_org_storage_totals () returns trigger language plpgsql as $$
declare
  v_org text;
begin
  v_org := coalesce(new.organisation, old.organisation);

  update core.organisations
  set
    total_virtual_size = (
      select coalesce(sum(u.total_virtual_size), 0)
      from core.users u
      where u.organisation = v_org and u.deleted_at is null
    ) + (
      select coalesce(sum(a.total_virtual_size), 0)
      from core.automated_services a
      where a.organisation = v_org and a.deleted_at is null
    ),
    total_real_size = (
      select coalesce(sum(u.total_real_size), 0)
      from core.users u
      where u.organisation = v_org and u.deleted_at is null
    ) + (
      select coalesce(sum(a.total_real_size), 0)
      from core.automated_services a
      where a.organisation = v_org and a.deleted_at is null
    ),
    total_entry_count = (
      select coalesce(sum(u.total_entry_count), 0)
      from core.users u
      where u.organisation = v_org and u.deleted_at is null
    ) + (
      select coalesce(sum(a.total_entry_count), 0)
      from core.automated_services a
      where a.organisation = v_org and a.deleted_at is null
    )
  where name = v_org;

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
