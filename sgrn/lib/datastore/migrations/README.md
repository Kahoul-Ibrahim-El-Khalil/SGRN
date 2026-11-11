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

The `007_lowercase_domain_namespace.sql` migration normalizes existing domain
labels and paths to lower-case, then prevents future mixed-case domain labels
or user-domain assignments. Resolve any case-folding collision reported by
the migration before retrying it.

`008_admin_ui_support.sql` hardens tenant/lifecycle/quota behaviour for the
admin console. It prints **NOTICES** that require operator follow-up:

- admin assignments / user-domain permissions that name a user of a
  **different organisation** (or a domain of another organisation) remain in
  the table — the new `trg_assert_same_org` guard blocks _new_ violating
  writes only. Clean up the reported rows manually.
- storage rows with a domain label that cannot be resolved to the owner's
  organisation keep `domain_id = NULL`; re-home those namespaces manually.
  NEW domain writes get `domain_id` derived automatically.

Behaviour changes after this migration:

- Suspending / soft-deleting a user or service, or suspending an
  organisation, terminates its live sessions immediately (reason `revoked`).
- `status = 'invited'` users can no longer sign in (`authenticate_user` and
  the inline sign-in query check `status = 'active'`, `deleted_at IS NULL`
  and the organisation's status).
- Byte-level storage quotas (actor, organisation and domain) are enforced at
  the `storage.file_objects` stage. Legacy byte totals were never advanced by
  uploads (+1 entry, +0 bytes); the migration recomputes owner and
  organisation aggregates once from live mappings.
- `core.domains` gained `storage_limit` / `entry_count_limit` namespace caps;
  they are admin-route-only (never client-writable through generated CRUD).

## Conventions

- Idempotent: safe to re-run (conditional DML, `IF NOT EXISTS` DDL,
  `CREATE OR REPLACE`). Verify with a trailing summary `SELECT`.
- No machine-specific rows (keys, ids). Document manual backfills as
  comments, provable from store metadata (e.g. multipart ETags end in `-N`).
- Kept **outside** `postgres/` on purpose: the SQL asset embedder globs
  `postgres/*.sql` into the init stream, where a migration would run in the
  wrong order on fresh installs.

## Namespace convention

Canonical authorities use dots for domain hierarchy and slashes for storage
paths. Organisation-bound users have a null domain; all other users must be
assigned to a lower-case domain. See the datastore README for the shell's
`@organisation-...` authority forms.
