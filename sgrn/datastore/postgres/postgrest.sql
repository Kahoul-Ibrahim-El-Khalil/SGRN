-- ============================================================
-- postgrest setup script
-- ============================================================
-- runs after views/init.sql, so all core.* and storage.* detail
-- views are guaranteed to exist when the postgrest.* proxies are created.
-- 1. api schema (postgrest entry point)
create schema if not exists postgrest;

-- 2. grant usage on the api schema to the postgrest roles
grant usage on schema postgrest to sgrn_datastore,
sgrn_postgrest;

-- ============================================================
-- core api views (thin proxies over internal detail views)
-- ============================================================
-- server time (public / health-check)
create or replace view postgrest.server_time as
select
  now() as current_time;

grant
select
  on postgrest.server_time to sgrn_datastore,
  sgrn_postgrest;

-- organisations
-- FIX #9: exposes total_virtual_size, total_real_size, storage_limit so
-- the API can surface quota information per organisation without extra joins.
create or replace view postgrest.organisations as
select
  *
from
  core.organisation_details;

grant
select
  on postgrest.organisations to sgrn_datastore,
  sgrn_postgrest;

-- domains
create or replace view postgrest.domains as
select
  *
from
  core.domain_details;

grant
select
  on postgrest.domains to sgrn_datastore,
  sgrn_postgrest;

-- users
-- FIX #7: exclude soft-deleted users by joining core.users for deleted_at.
-- core.user_details does not expose the password or deleted_at columns,
-- but we need the deleted_at filter — hence the inner join.
create or replace view postgrest.users as
select
  ud.*
from
  core.user_details ud
  join core.users u on u.id = ud.id
where
  u.deleted_at is null;

grant
select
  on postgrest.users to sgrn_datastore,
  sgrn_postgrest;

-- automated_services (no token_secret_hash ever exposed)
-- FIX #7: exclude soft-deleted services.
create or replace view postgrest.automated_services as
select
  id,
  organisation,
  name,
  description,
  token, -- public identifier; secret is never exposed via api
  domain,
  status,
  metadata,
  total_virtual_size,
  total_real_size,
  storage_limit,
  created_at,
  updated_at
from
  core.automated_services
where
  deleted_at is null;

-- FIX #7
grant
select
  on postgrest.automated_services to sgrn_datastore,
  sgrn_postgrest;

create or replace function postgrest.create_automated_service (
  name varchar(128),
  metadata jsonb default '{}',
  organisation text default null
) returns table (
  id int,
  organisation text,
  name varchar(128),
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz,
  domain text
) language sql security definer as $$
    select * from core.create_automated_service(name, metadata, organisation);
$$;

grant
execute on function postgrest.create_automated_service (varchar, jsonb, text) to sgrn_datastore,
sgrn_postgrest;

create or replace function postgrest.create_automated_service (name varchar(128), metadata jsonb default '{}') returns table (
  id int,
  organisation text,
  name varchar(128),
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz,
  domain text
) language sql security definer as $$
    select * from core.create_automated_service(name, metadata, null::text);
$$;

grant
execute on function postgrest.create_automated_service (varchar, jsonb) to sgrn_datastore,
sgrn_postgrest;

-- rotate automated service credentials
create or replace function postgrest.rotate_automated_service_credentials (
  automated_service_id int,
  rotate_token boolean default false
) returns table (
  id int,
  organisation text,
  name varchar(128),
  token uuid,
  token_secret text,
  status text,
  metadata jsonb,
  created_at timestamptz,
  updated_at timestamptz
) language sql security definer as $$
    select * from core.rotate_automated_service_credentials(automated_service_id, rotate_token);
$$;

grant
execute on function postgrest.rotate_automated_service_credentials (int, boolean) to sgrn_datastore,
sgrn_postgrest;
