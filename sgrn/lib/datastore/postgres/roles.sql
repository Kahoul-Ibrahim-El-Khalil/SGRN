do $$ begin if not exists (
  select
  from pg_roles
  where rolname = 'sgrn_datastore'
) then create role sgrn_datastore with login password '${POSTGRES_PASSWORD}';
else alter role sgrn_datastore with login password '${POSTGRES_PASSWORD}';
end if;
end $$;
grant connect on database ${POSTGRES_DB} to sgrn_datastore;
grant usage on schema core,
  storage to sgrn_datastore;
-- sgrn_datastore: full crud, direct table access (no PostgREST intermediary).
-- The datastore now reads/writes the underlying core.*/storage.* tables
-- itself, so it needs SELECT/INSERT/UPDATE/DELETE on them directly.
grant select,
  insert,
  update,
  delete on all tables in schema core,
  storage to sgrn_datastore;
grant usage,
  select,
  update on all sequences in schema core,
  storage to sgrn_datastore;
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
-- ============================================================
-- safety — strip public from all application tables.
-- structural changes (drop, alter table) require object ownership;
-- the role is not an owner, so they are implicitly blocked.
-- ============================================================
revoke all privileges on all tables in schema core,
storage
from public;
