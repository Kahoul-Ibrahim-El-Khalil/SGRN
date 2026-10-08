# Datastore SQL Migrations (Upgrades of Existing Databases)

`--init-db` **drops and recreates** the database from the embedded schema, so
fresh installs never need anything here. This directory is for **existing
deployments upgrading binaries**: when a release changes the schema, it ships
a numbered migration capturing exactly the delta.

## Applying

With the datastore **stopped**, as a superuser, in numeric order:

```bash
psql -h /tmp -U <superuser> -d sgrn -v ON_ERROR_STOP=1 \
  -f sgrn/lib/datastore/migrations/001_garage_provider_and_chunking_trace.sql

psql -h /tmp -U <superuser> -d sgrn -v ON_ERROR_STOP=1 \
  -f sgrn/lib/datastore/migrations/002_sha256_checksum.sql
```

Then boot the new binary. New code and schema move together: the current
upload path passes a SHA-256 checksum to `upsert_object`, so deployments that
install that binary must apply `002` first. Otherwise PostgreSQL interprets
the literal provider (`'GARAGE'`) as the old function's boolean compression
argument and rejects uploads.

## Conventions

- Idempotent: safe to re-run (conditional DML, `IF NOT EXISTS` DDL,
  `CREATE OR REPLACE`). Verify with a trailing summary `SELECT`.
- No machine-specific rows (keys, ids). Document manual backfills as
  comments, provable from store metadata (e.g. multipart ETags end in `-N`).
- Kept **outside** `postgres/` on purpose: the SQL asset embedder globs
  `postgres/*.sql` into the init stream, where a migration would run in the
  wrong order on fresh installs.
