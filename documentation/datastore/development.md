# Datastore — future development guide

This document is the **practical checklist** for extending SGRN: new SQL
tables, generated CRUD routes, React dashboard pages, and embedded web
assets. It states what the codegen path covers and what stays **out of
scope** (hand-written C++ instead).

Related references:

- HTTP contract for generated CRUD: [crud_views.md](crud_views.md)
- Generator mapping (manifest → C++): [`generators/README.md`](../../sgrn/lib/datastore/scripts/generators/README.md)
- Embedded web UIs: [`sgrn/web/README.md`](../../sgrn/web/README.md)

---

## What you are building (two tracks)

Most new **tenant-scoped table CRUD** features touch both tracks:

```text
Track A — Backend                         Track B — Frontend
─────────────────                         ────────────────────
postgres/schemas/*.sql                    sgrn/web/datastore/src/
  └─ tables, constraints                    └─ pages, components
postgres/views/*.sql (optional)           sgrn/web/datastore/src/backend/
crud/manifest.json                          └─ endpoints.ts, fetcher.ts
generate_orm.py + generate_views.py       bun run build (via CMake)
cmake --build → sgrn_datastore            → embedded in same binary
```

There is **no separate web deploy**: the dashboard is Zstd-compressed into
`web_assets.hpp` and served by Drogon from inside `sgrn_datastore`.

---

## Track A — SQL and generated handlers

### 1. Add or change the table

Edit DDL under `sgrn/lib/datastore/postgres/`:

| Location | Purpose |
|---|---|
| `schemas/core.sql`, `schemas/storage.sql` | Base tables |
| `views/core/*.sql`, `views/storage/*.sql` | Read-only views (optional safer `read_relation`) |
| `functions/*.sql` | Auth, registration, token rotation — not generated |
| `init.sql` | `\i` order; rarely edited unless adding a new schema file |

Requirements for **generated CRUD** (see [crud_views.md](crud_views.md)):

- Single-column primary key
- Tenant column: explicit in manifest, or auto-detected (`organisation` /
  FK to `core.organisations`)
- No reliance on joins at query time

Apply to a dev database:

```bash
sgrn_datastore --init-db
```

### 2. Declare API intent in the manifest

Edit `sgrn/lib/datastore/crud/manifest.json`:

- Add the table under `schemas.<schema>.tables.<name>`
- Set `operations` (`list`, `get`, `create`, `update`, `delete`, or `[]` to skip)
- Override `columns` only where defaults are wrong (`filter_ops`, `insertable`, `updatable`)
- Optionally point `read_relation` at a **view** that hides columns the table still has

Regenerate checked-in C++:

```bash
python3 sgrn/lib/datastore/scripts/generators/generate_orm.py --password "$POSTGRES_PASSWORD"
PGPASSWORD="$POSTGRES_PASSWORD" python3 sgrn/lib/datastore/scripts/generators/generate_views.py
clang-format -i sgrn/lib/datastore/src/handlers/generated/*.gen.hpp \
    sgrn/lib/datastore/src/handlers/generated/RegisteredViews.cpp
```

Review `git diff`, then `cmake --build <dir> --target sgrn_datastore`.

### 3. When a SQL view helps (still not a join engine)

`read_relation` may name a view instead of the base table so reads omit
sensitive columns without custom C++. Example views already in tree:

- `core.domain_details` — same columns as `core.domains` today
- `core.user_details` — richer user projection for internal/auth use

The engine still runs **`SELECT … FROM <one relation> WHERE tenant = $1`**
plus simple column filters. A view may `JOIN` internally in Postgres, but the
**HTTP layer never expresses joins** — only filters on whitelisted columns of
that one relation.

---

## Track B — frontend (React dashboard)

Sources: `sgrn/web/datastore/` (React + Vite + TypeScript).

### 1. Register API paths

Add constants in `src/backend/endpoints.ts` (grouped by area: `Admin…`,
`QueryList…`, `Storage…`). Generated CRUD paths follow
`/api/v1/<kebab-table>` — e.g. `LIST_DOMAINS: "/api/v1/domains"`.

### 2. Call the API

Use `authenticatedFetch` from `src/backend/api/fetcher.ts` (adds
`Authorization: Bearer` from session).

**List with filters** (PostgREST-style query params):

```ts
const params = new URLSearchParams({
  "name": "like.%25lab%25",
  "order": "name",
  "limit": "50",
});
const res = await authenticatedFetch(`/api/v1/domains?${params}`);
const rows = await res.json(); // JSON array
```

**Create / update** — JSON body with whitelisted fields only; see
[crud_views.md](crud_views.md).

### 3. Build UI

Add or extend pages under `src/pages/`. Existing patterns:

- Admin roster: `src/pages/admin/page.tsx` (mix of **hand-written** admin routes and generated list endpoints)
- Drive/storage: `src/pages/drive/` → [storage.md](storage.md) APIs
- Auth: `src/pages/signin/` → [auth.md](auth.md)

Shared types: `sgrn/typescript/types/` (`@sgrn/types`). The
`sgrn/typescript/datastore/` package today targets **automated-service**
clients, not dashboard CRUD — dashboard code uses local types + `fetch`.

### 4. Rebuild the embedded bundle

Frontend changes do **not** require Python codegen. Rebuild pulls Bun in via CMake:

```bash
cmake --build <build-dir> --target sgrn_datastore
# or explicitly:
cmake --build <build-dir> --target sgrn_dashboard_assets
```

---

## Out of scope for generated CRUD

Use a **hand-written** `IHandler` subclass (see `handlers/admin.hpp`,
`handlers/storage.hpp`, `handlers/auth.hpp`, `handlers/query.hpp`) when you need:

| Need | Why codegen cannot do it |
|---|---|
| **Multi-table joins** exposed as one API | Engine queries one `read_relation` only |
| **Aggregations** (`GROUP BY`, counts, sums) | No aggregate query builder |
| **Nested / embedded resources** | No sub-resource routing |
| **Cross-tenant or admin-only projections** | Tenant always from session; use admin handlers |
| **Complex validation** (password hashing, token rotation) | Use `functions/*.sql` + dedicated handlers |
| **`storage.*` ownership** | Tenant is `user_id` / `automated_service_id`, not `organisation` — `StorageApiHandler` |
| **Composite primary keys** | Generator hard-errors |
| **File upload / streaming / multipart** | Storage and auth paths |
| **Side effects** (email, Redis, Garage) | Business handlers |

`read_relation` on a view that hides columns is **in scope**. A view that
**joins** tables for convenience is only in scope if you treat the result as
a flat, filterable list with no client-side join language.

---

## Embedded web assets and why Bun is required

SGRN does not ship static files beside the binary in production. Both UIs are
embedded at **compile time**.

### Pipeline (datastore dashboard)

```text
sgrn/web/datastore/src/**          React/TS sources
        │
        ▼  bun install  (when package.json / bun.lock change)
        ▼  VITE_BASE_PATH=/datastore bun run build
sgrn/web/datastore/build/          Vite output (git-ignored)
        │
        ▼  generate_embedded_assets.py (--kind web, Zstd level 22)
<build>/…/dashboard_generated/web_assets.hpp
        │
        ▼  #include <web_assets.hpp>  →  registerDashboardAssets()
sgrn_datastore binary serves GET /index.html, /assets/* from .rodata
```

Gateway SPA follows the same pattern from `sgrn/web/gateway/dist/` into
`sgrn_gateway` (see [`sgrn/web/README.md`](../../sgrn/web/README.md)).

### Why Bun specifically

CMake **invokes Bun directly** — there is no Node/npm fallback in the build
graph:

| Step | Command (from CMake) |
|---|---|
| Install deps (dashboard) | `bun install` in `sgrn/web/datastore/` |
| Production bundle | `VITE_BASE_PATH=/datastore bun run build` |
| Gateway bundle | `bun run build` in `sgrn/web/gateway/` |

You need [Bun](https://bun.sh) on `PATH` when building any target that
embeds web assets. Python (`zstandard`) is also required for the compression
step; the SGRN micromamba env covers both.

### Runtime behavior

- Lookup keys are **virtual paths with a leading slash** (`/index.html`,
  `/assets/index-abc123.js`) in `EmbeddedAsset::virtual_path`
- Clients with `Accept-Encoding: zstd` get pre-compressed bytes from `.rodata`
- Dashboard is mounted under **`/datastore/`** in nginx (`VITE_BASE_PATH` at build time)
- Gateway patches `index.html` at runtime for subpath deploy (`X-Forwarded-Prefix`)

Details: [`generators/README.md` — Embedded VFS](../../sgrn/lib/datastore/scripts/generators/README.md#embedded-vfs-asset-generators-scripts).

### Local frontend iteration (optional)

For HMR during UI work you can run Vite dev server in `sgrn/web/datastore/`
with a proxy to the API — but **shipping** always goes through `cmake --build`
so the binary and embedded hashes stay aligned.

---

## End-to-end checklist: new tenant-scoped entity

Example: expose `core.widgets` in the dashboard.

| Step | Action |
|---|---|
| 1 | `CREATE TABLE core.widgets (…)` with PK + `organisation` FK |
| 2 | Optional: `CREATE VIEW core.widget_details AS SELECT …` if reads should hide columns |
| 3 | `sgrn_datastore --init-db` |
| 4 | Manifest entry: `operations`, `columns` / `filter_ops` |
| 5 | `generate_orm.py` + `generate_views.py`; review diff |
| 6 | `cmake --build` → verify `GET /api/v1/widgets` |
| 7 | `endpoints.ts` + page/components using `authenticatedFetch` |
| 8 | `cmake --build` again → embedded dashboard includes new JS |

If the feature needs joins, admin cross-tenant lists, or storage semantics →
stop at step 4 and implement `handlers/*.cpp` instead (document in
`documentation/datastore/*.md`).

---

## What remains / not unified yet

Honest gaps useful for planning:

| Area | Today | Likely future work |
|---|---|---|
| **Admin vs CRUD overlap** | e.g. user list uses `/api/v1/admin/users`, not generated `/api/v1/users` | Consolidate or document which surface is canonical |
| **TypeScript from manifest** | No codegen of TS clients from `crud/manifest.json` | Optional OpenAPI or TS generator |
| **Dashboard README** | Default Vite template text in `sgrn/web/datastore/README.md` | Replace with SGRN-specific dev notes |
| **Gateway docs assets** | `doc_assets.hpp` compiled; handler TBD | Wire man pages to HTTP |
| **Complex reporting** | Hand-written query/admin only | Dedicated read models or BI export, not CRUD generator |

The **intended** path for simple multi-tenant tables is fully wired: SQL →
manifest → `.gen.hpp` → React → Bun → embedded binary. Everything else stays
explicit hand-written C++ until a dedicated subsystem exists.
