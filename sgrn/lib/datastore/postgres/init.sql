\set on_error_stop true
\echo 'INITIALIZING SGRN DATABASE'

drop database if exists ${POSTGRES_DB} with (force);

CREATE DATABASE ${POSTGRES_DB}
    ENCODING    = 'UTF8'
    LC_COLLATE  = 'en_US.UTF-8'
    LC_CTYPE    = 'en_US.UTF-8';
\c ${POSTGRES_DB}

create extension if not exists pgcrypto;

\i schemas/init.sql
\i roles.sql
\i views/init.sql
\i functions/init.sql
\i postgrest.sql
\i seeding.sql
\i seed_test_automated_service.sql
\i seed_dev_service.sql

\echo 'SGRN DATABASE INITIALIZATION COMPLETE'
