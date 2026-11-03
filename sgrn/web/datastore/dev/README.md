# `sgrn/web/datastore/dev/`

Dev-only assets owned by the **datastore dashboard** dev environment.  
Everything here is used exclusively by `bun run dev` and is never shipped to production.

---

## What's here

| File        | Purpose                                                           |
| ----------- | ----------------------------------------------------------------- |
| `seed.sql`  | Extra test users and data inserted after the standard schema init |
| `README.md` | This file                                                         |

---

## `seed.sql`

Runs once, automatically, the first time `bun run dev` creates the `sgrn_dev` database.  
It is **idempotent** — every insert uses `ON CONFLICT DO NOTHING`, so re-running it is always safe.

To add more seed data (e.g. extra domains, API tokens, test records), just append more `INSERT … ON CONFLICT DO NOTHING` statements to this file. They will be picked up the next time the dev database is recreated.

---

## Dev database

| Setting                     | Value                                                              |
| --------------------------- | ------------------------------------------------------------------ |
| Database name               | `sgrn_dev`                                                         |
| Host / port                 | `127.0.0.1:5432` (override with `POSTGRES_HOST` / `POSTGRES_PORT`) |
| App role                    | `sgrn_datastore` / `sgrn_datastore`                                |
| Superuser used for creation | `postgres` (override with `POSTGRES_SUPERUSER`)                    |

Connect with psql:

```bash
psql -h 127.0.0.1 -U sgrn_datastore -d sgrn_dev
```

Or as superuser:

```bash
psql -h 127.0.0.1 -U postgres -d sgrn_dev
```

---

## Pre-seeded credentials

These accounts exist in every fresh `sgrn_dev` database:

| Email                 | Password           | Role  | Source         |
| --------------------- | ------------------ | ----- | -------------- |
| `admin@local.com`     | `adminroot`        | admin | `seeding.sql`  |
| `dev@local.com`       | `devpassword`      | user  | `dev/seed.sql` |
| `dev-admin@local.com` | `devadminpassword` | admin | `dev/seed.sql` |

The dev service (used for automated/API access) is seeded by `seed_dev_service.sql`:

| Token  | `1e78dbe3-1f5b-404f-89c1-ae0c07e98c5c`        |
| ------ | --------------------------------------------- |
| Secret | `FEcPKSxEuJzpst-MYcR8OlQWzFYxPoVDK9Sk3qmGn6A` |

---

## `bun run dev` does everything automatically

1. Checks whether `sgrn_dev` already exists in PostgreSQL.
2. If not: runs the full schema init sequence + `seed.sql` + migrations.
3. Starts `sgrn_datastore` on port 5001 (connected to `sgrn_dev`).
4. Starts the Vite HMR server on port 5173.

You just need a running PostgreSQL instance accessible at `127.0.0.1:5432`.
