# Datastore code generators

This directory holds every code generator for `sgrn_datastore`. The core
principle for both scripts is the same:

- The **live database is the source of truth for structure** — generators
  introspect it for column existence, Postgres types, and primary keys.
- API **intent** (which tables are exposed, which operations, which filters,
  which columns are searchable/writable) lives in the checked-in
  **`crud/manifest.json`** allow-list, read at generation time only.
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
  crud/manifest.json   API-intent allow-list (exposed tables, operations, filters) — EDIT HERE SECOND
  scripts/generators/   <-- you are here: generate_orm.py, generate_views.py, migrate_comments_to_manifest.py
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
scripts (+ `crud/manifest.json` for view intent) → checked-in generated
C++ → `cmake --build`. If generated code and schema ever disagree, the
schema wins; regenerate, don't hand-edit generated files.

Run the scripts from the repo root — their default input/output paths are
anchored to `sgrn/lib/datastore` (derived from the script location), not to
your CWD, so `python3 sgrn/lib/datastore/scripts/generators/generate_views.py`
works from anywhere. (The ORM script's `src/orm/models` default is likewise
absolute; only explicitly passed relative `--out`/`--manifest` paths resolve
against your CWD.)

## The full loop (schema change → running server)

```bash
micromamba activate SGRN
# Repo root for every command below.

# 1. Edit the DDL (postgres/schemas/*.sql, postgres/views/..., ...) if the
#    *structure* changes (new table/column), and edit crud/manifest.json
#    if the *API surface* changes (expose a table, enable an operation,
#    change a filter chain or a column's filter_ops / insertable /
#    updatable). Adding a column needs both; changing only intent needs
#    only the manifest.

# 2. Reinit a dev database from the edited schema:
sgrn_datastore --init-db        # drops + recreates, applies postgres/init.sql

# 3. Regenerate the Drogon ORM models from the live schema:
python3 sgrn/lib/datastore/scripts/generators/generate_orm.py --password "$POSTGRES_PASSWORD"
#    --host/--port default to 127.0.0.1:5432. --clean wipes src/orm/models
#    first (rarely needed). Needs `drogon_ctl` on PATH.

# 4. Regenerate the compile-time CRUD view handlers from the live schema:
PGPASSWORD="$POSTGRES_PASSWORD" \
python3 sgrn/lib/datastore/scripts/generators/generate_views.py
#    --host/--port/--dbname/--user default to 127.0.0.1:5432/sgrn/sgrn_datastore,
#    --password to "dracaeris" (same dev defaults as generate_orm.py; override
#    via --password or PGPASSWORD for real databases).
#    Needs `psycopg2` (pip install psycopg2-binary).
#    Only ever APPENDS to src/handlers/generated/RegisteredViews.cpp —
#    a registration line you delete stays deleted (see below).
#    Then normalize formatting of the generated headers:
clang-format -i sgrn/lib/datastore/src/handlers/generated/*.gen.hpp \
    sgrn/lib/datastore/src/handlers/generated/RegisteredViews.cpp

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

1. **Load `crud/manifest.json`** (`--manifest`, default `crud/manifest.json`
   relative to `sgrn/lib/datastore`) and validate it: unknown
   top-level/table/column keys, unknown operation names, unknown filter-op
   names, and any filter string with no matching `class *Filter : public
   drogon::HttpFilter<…>` declaration under `src/filters/` or
   `include/**/filters/` (scanned, never hardcoded) are all hard errors
   naming the offending manifest path. Filters are defined in C++ source;
   the manifest only *references* them.
2. **Connect** via `psycopg2` (`--host/--port/--dbname/--user`, password
   from `--password` or the `PGPASSWORD` env var, defaulting to the same
   dev credentials as `generate_orm.py`).
3. **Iterate the manifest allow-list** (`manifest["schemas"][…]["tables"]`,
   optionally narrowed with `--schemas`). A table absent from the manifest
   is NOT generated — there is no "generate everything except …" mode.
   `operations: []` (e.g. `core.sessions`; auth/session state is owned by
   hand-written code, never raw CRUD) generates nothing for that table.
4. **Build one view spec per table** (`build_view_spec()`). The live DB
   supplies structure only — column existence, `udt_name` → `FieldType`
   via `PG_TYPE_MAP` (unknown types warn and fall back to `Text`, which is
   always safe since every bind travels as text and Postgres casts
   implicitly), and the primary key. Everything else comes from the
   manifest. Anything inconsistent with the live DB is a hard error, never
   a silent skip:
   - manifest table/schema missing from the DB → error (fail the run);
   - manifest column missing from the live table → error (fail the run);
   - explicit `tenant_column` missing from the live table → error;
   - no primary key, or no tenant column (explicit `tenant_column`, else a
     column literally named `organisation`/`organisation_id`, else a column
     with a foreign key referencing `core.organisations`) → error. Tables
     that cannot satisfy this (`core.organisations` itself, all of
     `storage.*` whose ownership is `user_id`/`automated_service_id`-based
     and served by the hand-written `StorageApiHandler`, keyless views such
     as `core.user_details`) simply stay absent from the manifest.
   - columns named like `password`, `token_secret_hash`, … (see
     `SENSITIVE_COLUMNS`) are **omitted from the API surface entirely** —
     not readable, not filterable, not writable — even though they exist
     on the table, and listing one in the manifest is a hard error so a
     manifest edit can never accidentally expose a password hash. The
     engine's explicit column list enforces the omission.
5. **Map each kept column** from its manifest entry: `filter_ops` bitmask
   (`["eq", "like"]` → `Op::Eq | Op::Like`); **an omitted column entry
   means `filter_ops = 0`: the column is not searchable at all**
   (default-closed, deliberately stricter than PostgREST);
   `insertable`/`updatable` default to `true`, `false` closes the column
   for writes (the manifest spelling of the old per-column `readonly`).
6. **Emit** `<ClassName>View.gen.hpp` from `HEADER_TEMPLATE`: a
   `static constexpr std::array<Field, N> kFields` whitelist, a
   `static constexpr CrudViewSpec kSpec` (read/write relations, tenant
   column, PK + type, `default_order`, `max_limit`), and two route tables
   sized to the table's enabled `operations` — collection routes (`GET`
   list / `POST` create) plus item routes (`GET`/`PATCH`/`DELETE …/{id}`),
   each guarded by its resolved per-operation filter chain
   (`operation_filters` override, else `default_filters`). Routes use
   kebab-case (`user_domain_permissions` →
   `/api/v1/user-domain-permissions`). (`op_expr()` wraps single-operator
   columns in `static_cast<uint8_t>(…)` because a bare `Op::X` does not
   implicitly convert to the `uint8_t` bitmask field.)
7. **Rewrite `AllViews.gen.hpp`** (one `#include` per view, sorted to match
   `clang-format`'s `SortIncludes` so regen is diff-stable).
8. **Append-only registration**: `update_registration_file()` inserts
60.    `static <View> s_<table>;` lines *inside* `initGeneratedViews()` and
61.    **never removes a line**. To take a table out of the API, delete its
62.    line in `RegisteredViews.cpp` — re-running the generator will not
63.    resurrect it. If a table is later dropped from the DB, its stale line
64.    fails to compile, which is the signal to delete it.

### `crud/manifest.json` — the API-intent allow-list

The manifest is the source of truth for *intent* (the live DB is the source
of truth for *structure*). It is read at generation time only, never at
server runtime. Shape (`$schema_version: 1`):

```json
{
  "$schema_version": 1,
  "default_filters": {
    "list":   ["sgrn::datastore::filters::UserAuthFilter"],
    "get":    ["sgrn::datastore::filters::UserAuthFilter"],
    "create": ["sgrn::datastore::filters::UserAuthFilter"],
    "update": ["sgrn::datastore::filters::UserAuthFilter"],
    "delete": ["sgrn::datastore::filters::UserAuthFilter"]
  },
  "schemas": {
    "core": {
      "tables": {
        "domains": {
          "tenant_column": "organisation",
          "read_relation": "core.domains",
          "write_table": "core.domains",
          "default_order": "id",
          "max_limit": 500,
          "operations": ["list", "get", "create", "update", "delete"],
          "operation_filters": {
            "delete": ["sgrn::datastore::filters::UserAuthFilter",
                       "sgrn::datastore::filters::AdminFilter"]
          },
          "columns": {
            "name": { "filter_ops": ["eq", "like"] },
            "token": { "insertable": false, "updatable": false }
          }
        },
        "sessions": {
          "operations": []
        }
      }
    }
  }
}
```

Semantics:

- A table missing from the manifest entirely is NOT generated (allow-list).
  `operations: []` is the spelling for "never exposed as raw CRUD" (what a
  table-level `readonly` used to mean) — it documents the opt-out where a
  bare absence would look like an oversight.
- `operation_filters` overrides `default_filters` per-operation, per-table;
  unlisted operations fall back to `default_filters`. Every filter string
  must match a `class *Filter : public drogon::HttpFilter<…>` declaration
  in source, otherwise generation fails — e.g. `delete` above additionally
  requires `AdminFilter` (an existing C++ class; the manifest never defines
  new filter behavior).
- `columns` only needs entries for columns that deviate from the defaults
  (`filter_ops: []`, `insertable: true`, `updatable: true`); omit a column
  entirely to keep those defaults. `insertable`/`updatable: false` is the
  replacement for per-column `readonly`. Supported filter operators: `eq
  neq gt gte lt lte like in`.
- Structural overrides: explicit `tenant_column` (else auto-detected),
  explicit `read_relation` (for tables that should read from a view instead
  of themselves), `default_order` (defaults to the PK), `max_limit`
  (defaults to 500).
- `SENSITIVE_COLUMNS` (`password`, `token_secret_hash`, …) stay a
  code-level constant, not manifest-overridable: listing one is a hard
  error, and unlisted ones are hard-omitted from the API surface.

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
run. Fix `crud/manifest.json` (or, for structural needs, the template) and
regenerate instead.

### Exposing a brand-new table end-to-end

1. Add the table in the migration, with an `organisation` tenant column
   (or FK to `core.organisations`) and a primary key.
2. Add a `crud/manifest.json` entry for it: `operations`, per-column
   `filter_ops` / `insertable` / `updatable`, and any `operation_filters`
   overrides.
3. `--init-db`, re-run this script, `clang-format -i` the generated dir.
4. The new `static …View s_<table>;` line appears in `RegisteredViews.cpp`
   automatically (printed as `new table registered: …`). Rebuild — the
   route is live. To *hide* a table again, remove its manifest entry (or
   narrow its `operations`) and delete its line.

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
  constructs once, self-registers its routes (one per enabled operation)
  through the same `IHandler` mechanism as every hand-written handler, and
  every request executes in-process via `CrudViewEngine` on the existing
  `DbClient` — tenant scoping stays a bound SQL parameter the client can
  neither see nor override.

### Troubleshooting

- `psycopg2` import error → `pip install psycopg2-binary` (or use the
  SGRN conda env, which already provides it).
- `drogon_ctl: command not found` (ORM script) → run inside
  `micromamba activate SGRN`.
- Authentication failure against the dev DB → both scripts default to the
  dev credentials (`sgrn` / `sgrn_datastore` / `"dracaeris"`, same as
  `generate_orm.py`'s `DB_CONFIG_TEMPLATE`); pass the real `--password`
  (or export `PGPASSWORD` for `generate_views.py`).
- `manifest not found: …/crud/manifest.json` → pass an explicit
  `--manifest` path (the default is anchored to `sgrn/lib/datastore`,
  independent of your CWD).
- `unknown filter '…'` → the manifest references a filter class with no
  `class *Filter : public drogon::HttpFilter<…>` declaration under
  `src/filters/` or `include/**/filters/`; check the spelling (e.g. the
  admin gate is `…::AdminFilter`) or implement the filter in C++ first.
- `table '…' does not exist in the live database` / `column does not exist
  on the live table` → the manifest drifted from the schema: `--init-db`
  from the current migrations and re-run, or fix the manifest entry. These
  are hard errors, never silent skips.
- `no tenant column detected … set an explicit 'tenant_column'` → expected
  if you list `core.organisations` (the tenant root itself) or a
  `storage.*` table (ownership there is
  `user_id`/`automated_service_id`-based, served by the hand-written
  `StorageApiHandler`); remove the entry and implement a hand-written
  `IHandler` for those instead.
- `'…' is a SENSITIVE_COLUMNS member … cannot be listed in the manifest` →
  by design: password hashes and secrets are hard-omitted from the API
  surface; remove the entry.
- `skip (no operations enabled in manifest)` → expected for tables opted
  out entirely, e.g. `core.sessions` with `"operations": []` (auth/session
  state is owned by hand-written code, never raw CRUD).
- `warn: unmapped type '…' … using Text` → add the `udt_name` to
  `PG_TYPE_MAP` if the column needs ordering/filtering beyond text
  semantics.
- Regen shows a diff you didn't expect → check `git diff` on the
  *schema* and on `crud/manifest.json`: the generator is a pure function
  of the live DB plus the manifest; surprise diffs mean the dev DB drifted
  from the migration files or the manifest.

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

1. `TENANT_COLUMN_CANDIDATES`, `SENSITIVE_COLUMNS`, `PG_TYPE_MAP` — top of
   file; rename/restrict to your schema.
2. `crud/manifest.json` — the allow-list schema (`TOP_LEVEL_KEYS`,
   `TABLE_KEYS`, `COLUMN_KEYS`, `ALL_OPERATIONS`, `ALL_FILTER_OPS`); rename
   keys or operations to your convention and update your manifest to match.
   `scan_known_filters()` / `filter_source_dirs()` locate your filter
   classes — point them at your filter directories.
3. `HEADER_TEMPLATE` — your namespace, your `#include` path to the
   handler base, your default filter strings (`default_filters` fallback),
   your route prefix (currently `f"/api/v1/…"` in `build_view_spec()`),
   your `max_limit` default. The `RouteConfig` / `ItemRouteConfig` alias
   names must match whatever your `IHandler`-equivalent calls them.
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
  only the *manifest convention* (`crud/manifest.json` allow-list shape)
  is worth copying verbatim.
- `StorageApiHandler`, the metadata endpoints, `postgrest.*` leftovers —
  SGRN-specific consumers, not part of the generator runtime.
- The dashboard/TS bindings — they speak the HTTP grammar, so they keep
  working against any fork's routes, but they are not required by it.
