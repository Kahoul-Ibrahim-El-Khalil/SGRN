-- ============================================================
-- sgrn database initialization script
-- ============================================================
-- usage: psql -u postgres -f init.sql
-- note: run from the directory containing this file

-- stop on first error - very important!
\set on_error_stop true

\echo '============================================================'
\echo 'INITIALIZING SGRN DATABASE'
\echo '============================================================'

\echo '[1/10] Dropping old database (if exists)...'
drop database if exists ${POSTGRES_DB} with (force);

\echo '[2/10] Creating new database...'
CREATE DATABASE ${POSTGRES_DB}
    ENCODING    = 'UTF8'
    LC_COLLATE  = 'en_US.UTF-8'
    LC_CTYPE    = 'en_US.UTF-8';
-- FIX #3: Role creation is handled exclusively in roles.sql.
-- Creating roles here caused a silent credential collision: roles.sql's
-- `if not exists` branch was always skipped, leaving init.sql's hardcoded
-- password ('dracaeris') permanently in effect instead of roles.sql's value.

\echo '[3/10] Connecting to ${POSTGRES_DB} database...'
\c ${POSTGRES_DB}
\echo '[4/10] Enabling pgcrypto extension...'
create extension if not exists pgcrypto;

\echo '[5/10] Creating schemas and base tables...'
\i schemas/init.sql

\echo '[6/10] Defining roles and permissions...'
\i roles.sql

\echo '[7/10] Creating internal views (core / storage)...'
-- ⚠️  must run before postgrest.sql so core.* detail views already exist
--     when api.* views reference them.
\i views/init.sql

\echo '[8/10] Creating internal functions ...'
\i functions/init.sql

\echo '[9/10] Setting up postgrest api views...'
\i postgrest.sql

\echo '[10/10] Seeding initial data...'
\i seeding.sql

\echo '[11/12] Seeding integration test automated service...'
\i seed_test_automated_service.sql

\echo '[12/12] Seeding S7 dev service (fixed-credential gateway configs)...'
\i seed_s7_dev.sql

\echo '============================================================'
\echo '✅ SGRN DATABASE INITIALIZATION COMPLETE'
\echo '============================================================'
\echo ''
\echo 'Quick verification commands:'
\echo '  psql -u odahim -d ${POSTGRES_DB} -c "select schema_name from information_schema.schemata;"'
\echo '  psql -u odahim -d ${POSTGRES_DB} -c "\dn"'
\echo '  psql -u odahim -d ${POSTGRES_DB} -c "\dt *.*"'
