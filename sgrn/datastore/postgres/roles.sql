do $$ begin if not exists (
  select
  from pg_roles
  where rolname = 'sgrn_datastore'
) then create role sgrn_datastore with login password '${POSTGRES_PASSWORD}';
else alter role sgrn_datastore with login password '${POSTGRES_PASSWORD}';
end if;
if not exists (
  select
  from pg_roles
  where rolname = 'sgrn_postgrest'
) then create role sgrn_postgrest with login password '${POSTGRES_PASSWORD}';
else alter role sgrn_postgrest with login password '${POSTGRES_PASSWORD}';
end if;
end $$;
grant connect on database ${POSTGRES_DB} to sgrn_datastore,
  sgrn_postgrest;
grant usage on schema core,
  storage,
  postgrest to sgrn_datastore,
  sgrn_postgrest;
-- sgrn_datastore: full crud
grant select,
  insert,
  update,
  delete on all tables in schema core,
  storage,
  postgrest to sgrn_datastore;
grant usage,
  select,
  update on all sequences in schema core,
  storage,
  postgrest to sgrn_datastore;
-- sgrn_postgrest: no delete
grant select,
  insert,
  update on all tables in schema core,
  storage to sgrn_postgrest;
grant usage,
  select,
  update on all sequences in schema core,
  storage to sgrn_postgrest;
-- default privileges for future tables
alter default privileges in schema core,
storage
grant select,
  insert,
  update,
  delete on tables to sgrn_datastore;
alter default privileges in schema core,
storage
grant usage,
  select,
  update on sequences to sgrn_datastore;
alter default privileges in schema core,
storage
grant select,
  insert,
  update on tables to sgrn_postgrest;
alter default privileges in schema core,
storage
grant usage,
  select,
  update on sequences to sgrn_postgrest;
-- ============================================================
-- safety — strip public from all application tables.
-- structural changes (drop, alter table) require object ownership;
-- neither role is an owner, so they are implicitly blocked.
-- ============================================================
revoke all privileges on all tables in schema core,
storage
from public;