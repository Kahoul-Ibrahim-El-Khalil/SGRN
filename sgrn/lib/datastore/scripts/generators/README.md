# Datastore code generators

This directory holds every code generator for `sgrn_datastore`. The core
principle for both scripts is the same:

- The **live database is the source of truth** — generators introspect it,
  there is no separate manifest file to keep in sync.
- Generated output is **checked into the repo** and compiled like any other
  source file. The ordinary CMake build **never talks to a database**.
- After a schema change you re-run the generator(s) by hand, review the
  `git diff`, then recompile. The build does not do this for you, on purpose:
  generated code must always be reviewable before it ships.

## Where am I? (repository orientation for newcomers)

This file documents one corner in depth. If you are new to the repo,
read in this order — each step narrows the scope:

1. **Repo map** — `README.md` at the root (module table: `gateway`,
   `datastore`, `scl`, `s7shell`, …) plus `documentation/BUILD.md` for
   presets and `documentation/MODULES.md` for component boundaries.
2. **Datastore overview** — `sgrn/lib/datastore/README.md` (ingestion /
   persistence architecture, embedded dashboard).
3. **This directory** — you are here: the two scripts that generate
   datastore C++ from the live database (details below).

The datastore subtree, and where this directory fits in it:

```
sgrn/lib/datastore/
  postgres/      DDL + migrations (schemas/, views/, functions/) — EDIT HERE FIRST
  scripts/generators/   <-- you are here: generate_orm.py, generate_views.py
  src/orm/models/       ORM output (per-table Drogon Mapper classes)
  src/handlers/generated/  CRUD-view output (.gen.hpp + registry)
  src/{handlers,services,filters,plugins,query}/  hand-written C++
  include/sgrn/datastore/  public headers (query/, handlers/, ...)
  configs/           sgrn.json, nginx, systemd units, postgres configs
  client/ shell/     C++ client library + interactive shell (API consumers)
sgrn/apps/datastore/          the `sgrn_datastore` server entry point (main.cpp)
sgrn/apps/datastore_shell/    the shell entry point
sgrn/web/datastore/           React dashboard (separate TS world, same HTTP API)
```

Rule of thumb: schema truth flows one way —
`postgres/` → live dev DB (`sgrn_datastore --init-db`) → this directory's
scripts → checked-in generated C++ → `cmake --build`. If generated code
and schema ever disagree, the schema wins; regenerate, don't hand-edit
generated files.

Run both scripts with `sgrn/lib/datastore` as the working directory —
their default output paths (`src/orm/models`, `src/handlers/generated`)
are relative to it.

## The full loop (schema change → running server)

```bash
micromamba activate SGRN
cd sgrn/lib/datastore

# 1. Edit the DDL (postgres/schemas/*.sql, postgres/views/..., ...).
#    Filter intent lives as COMMENT ON COLUMN next to the column, e.g.
#      COMMENT ON COLUMN core.domains.name IS 'sgrn: filter=eq,like';
#    (full annotation reference below, under generate_views.py)

# 2. Reinit a dev database from the edited schema:
sgrn_datastore --init-db        # drops + recreates, applies postgres/init.sql

# 3. Regenerate the Drogon ORM models from the live schema:
python3 scripts/generators/generate_orm.py --password "$POSTGRES_PASSWORD"
#    --host/--port default to 127.0.0.1:5432. --clean wipes src/orm/models
#    first (rarely needed). Needs `drogon_ctl` on PATH.

# 4. Regenerate the compile-time CRUD view handlers from the live schema:
PGPASSWORD="$POSTGRES_PASSWORD" \
python3 scripts/generators/generate_views.py
#    --host/--port/--dbname/--user default to 127.0.0.1:5432/sgrn/sgrn_datastore.
#    Needs `psycopg2` (pip install psycopg2-binary).
#    Only ever APPENDS to src/handlers/generated/RegisteredViews.cpp —
#    a registration line you delete stays deleted (see below).
#    Then normalize formatting of the generated headers:
clang-format -i src/handlers/generated/*.gen.hpp \
    src/handlers/generated/RegisteredViews.cpp

# 5. Review, recompile, run:
git diff --stat          # generated code is always reviewable
cmake --build <build-dir> --target sgrn_datastore
```

---

## `generate_orm.py` — Drogon ORM models

### How it works

For each schema in `DEFAULT_SCHEMAS = ["core", "storage"]`, `generate_model_orm()`
does four things:

1. `mkdir -p src/orm/models/<schema>/`
2. Writes `<schema>/model.json`: a `drogon_ctl` connection file from
   `DB_CONFIG_TEMPLATE` — `rdbms`/`host`/`port`/`dbname`/`user`/`passwd`
   (overridable via `--host`/`--port`/`--password`; note there are **no**
   `--dbname`/`--user` flags, they are hardcoded to `sgrn`/`sgrn_datastore`),
   plus `"tables": []` (empty = auto-discover every table) and the schema.
3. Flushes/closes the file, then shells out to
   `drogon_ctl create model <dir>`, piping `y` to answer its
   "files will be overwritten, continue?" prompt.
4. `drogon_ctl` introspects `information_schema` (tables, columns, primary
   keys, foreign keys) and emits one Mapper-based ActiveRecord class per
   table: `<Table>.h` / `<Table>.cc` in namespace
   `drogon_model::sgrn::<schema>` (e.g. `core::Users`, `storage::Files`).

Hand-written code consumes these via `drogon::orm::CoroMapper<T>`
(e.g. `CoroMapper<drogon_model::sgrn::core::Users>` in
`src/handlers/admin.cpp`), i.e. type-safe SELECT/INSERT/UPDATE without
hand-written SQL for the hot paths.

### CLI reference

| Flag | Default | Notes |
|---|---|---|
| `--clean` | off | `rm -rf src/orm/models` first. Rarely needed; prefer incremental runs so unrelated models don't churn. |
| `--password` | `"dracaeris"` | Practically always overridden with the real DB password. |
| `--host` | `127.0.0.1` | |
| `--port` | `5432` | |

### What it generates (checked in)

```
src/orm/models/
  core/{AutomatedServices,Domains,Organisations,Sessions,UserDomainPermissions,Users}.{h,cc}
  core/model.json
  storage/{Directories,Files,Formats,Objects}.{h,cc}
  storage/model.json
```

`model.json` files are `drogon_ctl` inputs that happen to be committed
alongside their outputs — keep them, they document exactly which connection
produced the models.

### Build-system integration

`src/CMakeLists.txt` globs them with `CONFIGURE_DEPENDS`:

```cmake
file(GLOB_RECURSE ORM_SOURCES CONFIGURE_DEPENDS orm/models/*.cc)
```

so adding/regenerating a model file triggers an automatic CMake
re-configure on the next build — no manual reconfigure step. The glob feeds
a dedicated `sgrn_datastore_orm` library (compiled with `-w`: generated code
is exempt from the project's warnings), linked `PUBLIC` into the fat
`sgrn_datastore_lib`, so every handler/service sees the models. `BUILD_ORM`
can switch the whole thing off; static vs shared follows
`SGRN_BUILD_STATIC` like everything else.

---

## `generate_views.py` — compile-time CRUD view handlers

This is the codegen half of the in-process REST-over-Postgres layer that
replaced the external PostgREST process: per-table HTTP handlers are
*generated at compile time* from the live schema instead of being
interpreted from query strings at runtime.

### How it works

1. **Connect** via `psycopg2` (`--host/--port/--dbname/--user`, password
   from `--password` or the `PGPASSWORD` env var).
2. **Enumerate tables** per `--schemas` (default `core`, `storage`) from
   `information_schema.tables`.
3. **Build one view spec per table** (`build_view_spec()`), with an
   explicit skip policy — every skip prints its reason:
   - table comment parses to `sgrn: readonly` → skip (e.g. `core.sessions`;
     auth/session state is owned by hand-written code, never raw CRUD);
   - no primary key (from `table_constraints` + `key_column_usage`) → skip
     (this is what excludes *views* such as `core.user_details` and
     `storage.file_details`);
   - no tenant column → skip with "write a manual handler". Detection is
     a column literally named `organisation`/`organisation_id`, falling
     back to a column with a foreign key referencing
     `core.organisations` (this is what excludes `core.organisations`
     itself plus all of `storage.*`, whose ownership is
     `user_id`/`automated_service_id`-based and served by the
     hand-written `StorageApiHandler`);
   - columns named like `password`, `token_secret_hash`, … (see
     `SENSITIVE_COLUMNS`) are **omitted from the API surface entirely** —
     not readable, not filterable, not writable — even though they exist
     on the table. The engine's explicit column list enforces this.
4. **Map each kept column**: Postgres `udt_name` → `FieldType` via
   `PG_TYPE_MAP` (unknown types warn and fall back to `Text`, which is
   always safe since every bind travels as text and Postgres casts
   implicitly); the column's `COMMENT` (read through the
   `information_schema.columns` + `pg_statio_all_tables` +
   `pg_description` join) is parsed by `parse_tags()`:
   - `sgrn: filter=eq,like` → `filter_ops` bitmask (`Op::Eq | Op::Like`);
     **no comment means `filter_ops = 0`: the column is not searchable
     at all** (default-closed, deliberately stricter than PostgREST);
   - `sgrn: readonly` → `insertable = false, updatable = false`;
     otherwise both default to `true`.
5. **Emit** `<ClassName>View.gen.hpp` from `HEADER_TEMPLATE`: a
   `static constexpr std::array<Field, N> kFields` whitelist, a
   `static constexpr CrudViewSpec kSpec` (read/write relations, tenant
   column, PK + type, default order, `max_limit = 500`), and two route
   tables — 2 collection routes (`GET` list / `POST` create) plus 3 item
   routes (`GET`/`PATCH`/`DELETE …/{id}`), all behind
   `UserAuthFilter`. Routes use kebab-case (`user_domain_permissions` →
   `/api/v1/user-domain-permissions`). (`op_expr()` wraps single-operator
   columns in `static_cast<uint8_t>(…)` because a bare `Op::X` does not
   implicitly convert to the `uint8_t` bitmask field.)
6. **Rewrite `AllViews.gen.hpp`** (one `#include` per view, sorted to match
   `clang-format`'s `SortIncludes` so regen is diff-stable).
7. **Append-only registration**: `update_registration_file()` inserts
   `static <View> s_<table>;` lines *inside* `initGeneratedViews()` and
   **never removes a line**. To take a table out of the API, delete its
   line in `RegisteredViews.cpp` — re-running the generator will not
   resurrect it. If a table is later dropped from the DB, its stale line
   fails to compile, which is the signal to delete it.

### Annotation mini-language (`COMMENT ON COLUMN`, next to the DDL)

Reference examples live in `postgres/schemas/core.sql` under
"sgrn: CRUD view annotations":

```sql
COMMENT ON COLUMN core.organisations.name IS 'sgrn: filter=eq,like';
COMMENT ON COLUMN core.organisations.storage_limit IS 'sgrn: filter=eq,gte,lte';
COMMENT ON COLUMN core.automated_services.token IS 'sgrn: readonly';
COMMENT ON TABLE core.sessions IS 'sgrn: readonly';
```

Supported operators: `eq neq gt gte lt lte like in`, comma-separated
after `filter=`; clauses combine with `;`. Anything else in the comment
is ignored, so the tags coexist with human prose.

### What it generates (checked in)

```
src/handlers/generated/
  DomainsView.gen.hpp
  UsersView.gen.hpp
  AutomatedServicesView.gen.hpp
  UserDomainPermissionsView.gen.hpp
  AllViews.gen.hpp            # sorted aggregate include
  RegisteredViews.cpp         # hand-curated registry (append-only)
  RegisteredViews.hpp         # hand-written decl of initGeneratedViews()
```

A minimal generated header (`DomainsView.gen.hpp`, 1 filterable column)
shows the whole shape:

```cpp
// GENERATED by generate_views.py — DO NOT EDIT.
// Source: core.domains
#pragma once
#include <sgrn/datastore/handlers/CrudViewHandler.hpp>

#include <array>

namespace sgrn::datastore::handlers::query
{
...
class DomainsView : public CrudViewHandler<DomainsView> {
public:
    static constexpr std::array<Field, 1> kFields = {{
        {"name", FieldType::Text, Op::Eq | Op::Like, true, true},
    }};

    static constexpr CrudViewSpec kSpec{
        .read_relation = "core.domains",
        .write_table = "core.domains",
        .tenant_column = "organisation",
        .pk = {"id", FieldType::Int},
        .fields = kFields,
        .default_order = "id",
        .max_limit = 500,
    };

    DomainsView()
        : CrudViewHandler(this, kRoutes, kItemRoutes) {
    }

private:
    static inline const std::array<RouteConfig, 2> kRoutes = {{
        {"/api/v1/domains", &DomainsView::handleList, {drogon::Get}, {{"sgrn::datastore::filters::UserAuthFilter"}}},
        {"/api/v1/domains", &DomainsView::handleCreate, {drogon::Post}, {{"sgrn::datastore::filters::UserAuthFilter"}}},
    }};
    static inline const std::array<ItemRouteConfig, 3> kItemRoutes = {{
        {"/api/v1/domains/{id}", &DomainsView::handleGet, {drogon::Get}, {{"sgrn::datastore::filters::UserAuthFilter"}}},
        {"/api/v1/domains/{id}", &DomainsView::handleUpdate, {drogon::Patch}, {{"sgrn::datastore::filters::UserAuthFilter"}}},
        {"/api/v1/domains/{id}", &DomainsView::handleDelete, {drogon::Delete}, {{"sgrn::datastore::filters::UserAuthFilter"}}},
    }};
};

} // namespace sgrn::datastore::handlers::query
```

Never hand-edit a `.gen.hpp` — your change is overwritten on the next
run. Fix the schema comment (or, for structural needs, the template) and
regenerate instead.

### Exposing a brand-new table end-to-end

1. Add the table + `COMMENT ON COLUMN … IS 'sgrn: filter=…'` lines in the
   migration, with an `organisation` tenant column (or FK to
   `core.organisations`) and a primary key.
2. `--init-db`, re-run this script, `clang-format -i` the generated dir.
3. The new `static …View s_<table>;` line appears in `RegisteredViews.cpp`
   automatically (printed as `new table registered: …`). Rebuild — the
   route is live. To *hide* a table again, delete its line.

### Build-system integration

| Generated output | Picked up by | Compiled into |
|---|---|---|
| `src/handlers/generated/RegisteredViews.cpp` | `HANDLERS_SOURCES` glob (`handlers/*.cpp`, `CONFIGURE_DEPENDS`, recurses into `generated/`) | `sgrn_datastore_lib` |
| `*.gen.hpp` / `AllViews.gen.hpp` | `#include`d by `RegisteredViews.cpp`; `src/` is on the lib's `PUBLIC` include path | (headers, same TU) |
| `src/query/CrudViewEngine.cpp` (hand-written engine, not generated) | `SGRN_QUERY_SOURCES` glob in `cmake/sgrn_views_codegen.cmake` | `sgrn_datastore_lib` via `target_sources` |

- `CONFIGURE_DEPENDS` on every glob means adding, deleting, or regenerating
  a file triggers an automatic CMake re-configure on the next build —
  generated-file churn never needs a manual reconfigure.
- `cmake --build <dir> --target sgrn_generate_views` re-runs this script
  against the live DB (needs `PGPASSWORD` in the environment). It is
  deliberately **not** part of the default build graph: builds must never
  require database connectivity.
- Hand-written once, never regenerated: `CrudViewSpec.hpp`,
  `CrudViewEngine.{hpp,cpp}`, `CrudViewHandler.hpp`,
  `IHandler::item_route_config`, `core/db.hpp`, `RegisteredViews.hpp`.
  The generator only owns the `.gen.hpp` files, `AllViews.gen.hpp`, and
  *appending* to `RegisteredViews.cpp`.
- At runtime, `initHandlers()` (`include/sgrn/datastore/init/handlers.hpp`)
  calls `query::initGeneratedViews()`: each function-local static view
  constructs once, self-registers its five routes through the same
  `IHandler` mechanism as every hand-written handler, and every request
  executes in-process via `CrudViewEngine` on the existing `DbClient` —
  tenant scoping stays a bound SQL parameter the client can neither see
  nor override.

### Troubleshooting

- `psycopg2` import error → `pip install psycopg2-binary` (or use the
  SGRN conda env, which already provides it).
- `drogon_ctl: command not found` (ORM script) → run inside
  `micromamba activate SGRN`.
- `--password is required` → pass `--password` or export `PGPASSWORD`.
- `skip (table marked readonly)` → expected for tables opted out
  entirely, e.g. `core.sessions` (auth/session state is owned by
  hand-written code, never raw CRUD).
- `skip (no tenant column detected — write a manual handler)` → expected
  for `core.organisations` (the tenant root itself) and all of `storage.*`
  (ownership there is `user_id`/`automated_service_id`-based, served by
  the hand-written `StorageApiHandler`); implement a hand-written
  `IHandler` for those.
- `skip (no primary key)` → expected for views (`user_details`,
  `file_details`, …); views are read-models, not CRUD roots.
- `warn: unmapped type '…' … using Text` → add the `udt_name` to
  `PG_TYPE_MAP` if the column needs ordering/filtering beyond text
  semantics.
- Regen shows a diff you didn't expect → check `git diff` on the
  *schema*: the generator is a pure function of the live DB; surprise
  diffs mean the dev DB drifted from the migration files.

---

## Reusing these generators in another project (fork guide)

Both scripts were built to be liftable: the database side depends only on
stock Postgres catalogs, and the C++ side depends only on Drogon plus a
small, enumerated seam of SGRN helpers. (Note: this repo is LGPL-3.0 —
see `LICENSE` at the root; keep the notices if you fork.)

### What travels together

**For `generate_orm.py`** — just the script. Its only inputs are a live
Postgres and `drogon_ctl` on `PATH`; its outputs are vanilla Drogon
Mapper models consumed through `drogon::orm::CoroMapper<T>`. Porting is:
`DB_CONFIG_TEMPLATE` (dbname/user), `DEFAULT_SCHEMAS`, `DEFAULT_TARGET_DIR`.

**For `generate_views.py`** — the script *plus* its runtime, which is
deliberately small. Copy these files as a unit:

```
# Generator (this directory)
scripts/generators/generate_views.py
# Runtime (hand-written once, never regenerated)
include/sgrn/datastore/query/CrudViewSpec.hpp      # stdlib only — no deps at all
include/sgrn/datastore/query/CrudViewEngine.hpp    # + drogon Http
src/query/CrudViewEngine.cpp                        # + fmt, jsoncpp, see below
include/sgrn/datastore/handlers/CrudViewHandler.hpp # + IHandler, respond
include/sgrn/datastore/utils/IHandler.hpp           # + route_utils.hpp (drogon only)
include/sgrn/datastore/core/db.hpp                  # drogon ORM only
```

The remaining `#include`s inside those files are the seam you shim or
port (all under `include/sgrn/datastore/`):

| Needed by the runtime | Provides | Fork option |
|---|---|---|
| `utils/respond.hpp` | `createJsonResponse` / `createErrorResponse` | port (small), or rewrite the ~10 call sites to raw `drogon::HttpResponse` |
| `utils/safe_access.hpp` (`core::getDbClient`) | default `DbClientPtr` from `drogon::app()` | 5-line shim; note the SGRN header also pulls Redis helpers you can drop |
| `BackendError` / `Result` (`sgrn/Result.hpp` et al.) | error plumbing | port the two tiny headers, or typedef onto your own error type |
| auth filter *name* (string in the template) | request gate | keep the string, register your own filter under that name |
| session contract | tenant value | `CrudViewHandler::tenantOf()` reads attribute `session_json["user"]["organisation"]` — adapt that one function to your session shape |

`fetch_*()` SQL uses only `information_schema` + `pg_catalog`, so it
works against any Postgres — no SGRN-specific catalogs involved.

### Adaptation checklist (`generate_views.py`)

All fork-coupling points are module-level constants or the two clearly
marked functions — no archaeology needed:

1. `TENANT_COLUMN_CANDIDATES`, `SENSITIVE_COLUMNS`, `PG_TYPE_MAP`,
   `DEFAULT_SCHEMAS` — top of file; rename/restrict to your schema.
2. `parse_tags()` — the `r"sgrn:\s*(.+)"` regex is the annotation prefix;
   change the prefix (and the `filter=`/`readonly` keywords if you like)
   and update your DDL comments to match.
3. `HEADER_TEMPLATE` — your namespace, your `#include` path to the
   handler base, your auth-filter string, your route prefix (currently
   `f"/api/v1/…"` in `build_view_spec()`), your `max_limit`. The
   `RouteConfig` / `ItemRouteConfig` alias names must match whatever your
   `IHandler`-equivalent calls them.
4. `build_view_spec()` — class/route naming (`CamelCase + "View"`,
   kebab-case routes) and the `read_relation == write_table` default;
   point `read_relation` at a safe view if your tables carry secrets
   (the SGRN tree does this implicitly via field omission — see
   `SENSITIVE_COLUMNS`).
5. `update_registration_file()` + the `initGeneratedViews()` handshake —
   call it once from your app startup after config load, before
   `app().run()`, mirroring `initHandlers()`.
6. CMake: copy the `file(GLOB … CONFIGURE_DEPENDS …)` pattern and the
   manual `sgrn_generate_views`-style custom target from
   `cmake/sgrn_views_codegen.cmake`, adjusted to your paths. Keep
   generation out of the default build graph.

### What NOT to take

- `postgres/` — the SGRN schema, roles, and seeds. Bring your own tables;
  only the *annotation convention* (`COMMENT ON COLUMN … 'prefix: …'`)
  is worth copying verbatim.
- `StorageApiHandler`, the metadata endpoints, `postgrest.*` leftovers —
  SGRN-specific consumers, not part of the generator runtime.
- The dashboard/TS bindings — they speak the HTTP grammar, so they keep
  working against any fork's routes, but they are not required by it.
