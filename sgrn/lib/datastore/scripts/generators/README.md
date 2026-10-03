# SGRN code generators

This document is the exhaustive reference for **every code generator** in
the SGRN tree: datastore DB codegen (this directory), embedded VFS assets
(`scripts/generate_embedded_assets.py`), and how each maps its inputs into
C++ source.

Two different philosophies coexist on purpose:

| Category | When it runs | Source of truth | Output checked in? |
|---|---|---|---|
| **Datastore DB codegen** (`generate_orm.py`, `generate_views.py`) | Manual, after schema/API changes | Live Postgres + `crud/manifest.json` | **Yes** — review the `git diff` before commit |
| **Embedded VFS assets** (`generate_embedded_assets.py`) | **Automatic** during `cmake --build` | Frontend build dirs / `postgres/` / `configs/` on disk | **No** — headers land in the build dir (`*_generated/`) |

### Generator catalog

| Script | Location | Primary input | Primary output | Mapping |
|---|---|---|---|---|
| `generate_views.py` | here | `crud/manifest.json` + live DB | `src/handlers/generated/*.gen.hpp` | [Manifest → C++ views](#manifest-table-entry--c-output-reference) |
| `generate_orm.py` | here | live DB (`core`, `storage`) | `src/orm/models/<schema>/*.{h,cc}` | [Live table → ORM](#live-table--c-orm-output-reference) |
| `migrate_comments_to_manifest.py` | here | legacy `sgrn:` SQL comments | `crud/manifest.json` | one-off; see script header |
| `generate_embedded_assets.py` | `scripts/` | any directory tree | per-file `*.hpp` + master `*_assets.hpp` | [Source file → VFS entry](#source-file--c-vfs-output-reference) |
| `generate_web_headers.py` | `scripts/` | (shim) | same as `--kind web` | [Web shim](#generate_web_headerspy--compatibility-shim) |
| `embded_web.py` | `scripts/` | explicit file list | single `web.hpp` | [Legacy one-off embedder](#embded_webpy--legacy-manual-embedder) |

## Where am I? (repository orientation for newcomers)

This file documents one corner in depth. If you are new to the repo,
read in this order — each step narrows the scope:

1. **Repo map** — `README.md` at the root (module table: `gateway`,
   `datastore`, `scl`, `s7shell`, …) plus `documentation/BUILD.md` for
   presets and `documentation/MODULES.md` for component boundaries.
2. **Datastore overview** — `sgrn/lib/datastore/README.md` (ingestion /
   persistence architecture, embedded dashboard).
3. **HTTP contract for generated CRUD** —
   [`documentation/datastore/crud_views.md`](../../../../../documentation/datastore/crud_views.md)
   (what each operation expects on the wire: auth, query filters, JSON
   bodies, responses, errors).
4. **Future development checklist** —
   [`documentation/datastore/development.md`](../../../../../documentation/datastore/development.md)
   (SQL → manifest → frontend → embedded web assets; joins and complex
   queries out of scope).
5. **This file** — exhaustive generator reference (datastore DB codegen +
   embedded VFS assets).

Where generators live and what they feed:

```
scripts/
  generate_embedded_assets.py   unified Zstd VFS generator (web/sql/config/…)
  generate_web_headers.py       thin shim → embedded_assets --kind web
  embded_web.py                 legacy manual embedder (not wired into CMake)

sgrn/lib/datastore/
  postgres/                     SQL source  ──► sql_assets.hpp (build-time)
  configs/                      defaults    ──► config_assets.hpp (build-time)
  crud/manifest.json            API intent  ──► *.gen.hpp (manual regen)
  scripts/generators/           generate_orm.py, generate_views.py
  src/orm/models/               ORM output (checked in, manual regen)
  src/handlers/generated/       CRUD views (checked in, manual regen)
sgrn/web/datastore/             React SPA   ──► web_assets.hpp (build-time)
sgrn/web/gateway/               Svelte SPA  ──► web_assets.hpp (build-time)
documentation/gateway/          man pages   ──► doc_assets.hpp (build-time)
```

Runtime types shared by every VFS bundle:
`sgrn/lib/core/include/sgrn/assets/EmbeddedAsset.hpp` (`EmbeddedAsset`,
`AssetRegistry`, `AssetKind`).

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

# 2. Reinit a dev database from the edited schema (no compilation required):
python3 init-db.py              # drops + recreates, applies postgres/init.sql
# (or `sgrn_datastore --init-db` if binary is compiled)

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

Unlike `generate_views.py`, this script does **not** read
`crud/manifest.json`. It has no allow-list: every table in the configured
Postgres schemas is modeled.

### What each input controls

| Input | Read when | Controls in generated C++ |
|---|---|---|
| Live Postgres (`information_schema`) | Generation only | Every table/column in the schema, PKs, FKs, Postgres types |
| `DEFAULT_SCHEMAS` (`core`, `storage`) in the script | Generation time | Which schema subdirectories are emitted under `src/orm/models/` |
| `DB_CONFIG_TEMPLATE` + CLI `--host`/`--port`/`--password` | Generation time | Connection file written to `<schema>/model.json` (dbname/user hardcoded to `sgrn`/`sgrn_datastore`) |

There is **no manifest mapping** for ORM output — the live schema is the
only intent input.

### Generation behavior (exact algorithm)

For each schema in `DEFAULT_SCHEMAS = ["core", "storage"]`,
`generate_model_orm()`:

1. `mkdir -p src/orm/models/<schema>/`
2. Writes `<schema>/model.json`: a `drogon_ctl` connection file from
   `DB_CONFIG_TEMPLATE` — `rdbms`/`host`/`port`/`dbname`/`user`/`passwd`
   (overridable via `--host`/`--port`/`--password`; note there are **no**
   `--dbname`/`--user` flags, they are hardcoded to `sgrn`/`sgrn_datastore`),
   plus `"tables": []` (empty = auto-discover **every** table in the schema)
   and `"schema": "<schema>"`.
3. Flushes/closes the file, then shells out to
   `drogon_ctl create model <dir>`, piping `y` to answer its
   "files will be overwritten, continue?" prompt.
4. `drogon_ctl` introspects `information_schema` (tables, columns, primary
   keys, foreign keys) and emits one Mapper-based ActiveRecord class per
   table.

### Live table → C++ ORM output (reference)

| Live Postgres object | Generated files | C++ type |
|---|---|---|
| `core.users` table | `src/orm/models/core/Users.h`, `Users.cc` | `drogon_model::sgrn::core::Users` |
| `storage.files` table | `src/orm/models/storage/Files.h`, `Files.cc` | `drogon_model::sgrn::storage::Files` |
| each column on the table | `Users::Cols::_<column>`, getter/setter members, SQL bind helpers | column name preserved (`first_name` → `_first_name`, `getFirstName()`, …) |
| primary key | `getPrimaryKey()`, `insert()`/`update()` WHERE clause | from live constraint, not configurable |
| foreign keys | relation helpers on the Mapper | from live FK metadata |

Naming rule: table name → PascalCase class name (`user_domain_permissions`
→ `UserDomainPermissions`). **All columns** on the table are included,
including sensitive ones (`password`, …) — ORM models are for internal
server code, not the public HTTP API. The HTTP surface is separately
gated by `generate_views.py` + `crud/manifest.json`.

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

### What each input controls

| Input | Read when | Controls in generated C++ |
|---|---|---|
| `crud/manifest.json` | Generation only (never at server runtime) | Which tables exist as views, which CRUD operations are routed, filter chains per operation, per-column filter/write flags, structural overrides (`read_relation`, `tenant_column`, …) |
| Live Postgres (`information_schema`, `pg_catalog`) | Generation only | Table/column existence, column order, `udt_name` → `FieldType`, primary key column name |
| C++ filter sources (`src/filters/`, `include/**/filters/`) | Generation only (scanned, not compiled) | Allow-list of filter class names the manifest may reference |

The generator is a **pure function of (manifest + live DB structure)**.
If regen produces a surprise diff, one of those inputs drifted.

### Generation behavior (exact algorithm)

For every `manifest["schemas"][schema]["tables"][table]` entry (optionally
filtered by `--schemas`):

1. **Validate manifest shape** (before any DB work): unknown keys, bad
   types, duplicate operations, filter names with no matching
   `class *Filter : public drogon::HttpFilter<…>` in source, and
   `operation_filters` keys for operations not listed in `operations` are
   all hard errors with a manifest path in the message.
2. **Resolve operations**: `operations` defaults to all five CRUD ops when
   omitted. `operations: []` prints `skip (no operations enabled in
   manifest): schema.table` and emits **nothing** for that table (no
   `.gen.hpp`, no registration line).
3. **Introspect live table** (hard error if any check fails):
   - table must exist;
   - must have exactly one primary-key column (composite PKs are unsupported);
   - tenant column must resolve (see [Tenant column resolution](#tenant-column-resolution));
   - every column named under `columns` in the manifest must exist on the
     live table and must not be in `SENSITIVE_COLUMNS`.
4. **Build the field whitelist** (`kFields`): iterate live columns in
   `ordinal_position` order, **excluding** PK, tenant column, and
   `SENSITIVE_COLUMNS`. For each remaining column, merge DB type with
   manifest column config (see [Column defaults](#column-defaults-and-kfields-membership)).
5. **Build route tables** (`kRoutes`, `kItemRoutes`): one entry per enabled
   operation, always in canonical order (`list`, `create`, then `get`,
   `update`, `delete`) regardless of how the manifest lists them.
6. **Emit** `src/handlers/generated/<ClassName>.gen.hpp` from
   `HEADER_TEMPLATE`.
7. After all tables: **rewrite** `AllViews.gen.hpp` (sorted `#include`s)
   and **append** any missing `static <ClassName> s_<table>;` lines to
   `RegisteredViews.cpp` (never remove existing lines).

Tables **absent from the manifest are never generated**. There is no
"generate everything except …" mode.

### Manifest table entry → C++ output (reference)

One manifest table object under `schemas.<schema>.tables.<table>` produces
**one** generated handler class and **at most one** registration line.

#### Naming and file layout

| Manifest path | Generated artifact |
|---|---|
| `schemas.core.tables.domains` | `DomainsView.gen.hpp` — class `DomainsView` in namespace `sgrn::datastore::handlers::query` |
| (same) | HTTP prefix `/api/v1/domains` (table name, `_` → `-`, prefixed with `/api/v1/`) |
| (same) | `RegisteredViews.cpp`: `static DomainsView s_domains;` (appended on first generation only) |
| (aggregate) | `AllViews.gen.hpp`: `#include "DomainsView.gen.hpp"` (rewritten every run, sorted) |

Class name rule: split table name on `_`, capitalize each segment, append
`View` — `user_domain_permissions` → `UserDomainPermissionsView`.

#### Table-level keys → `CrudViewSpec` and routes

| Manifest key | Default when omitted | Generated C++ |
|---|---|---|
| `operations` | all five: `list`, `get`, `create`, `update`, `delete` | Which entries appear in `kRoutes` / `kItemRoutes`. Empty list → skip table entirely. |
| `tenant_column` | auto-detected (see below) | `kSpec.tenant_column = "…"` — bound server-side on every query, never client-settable |
| `read_relation` | `"<schema>.<table>"` | `kSpec.read_relation = "…"` — relation `SELECT` reads from (may be a view) |
| `write_table` | `"<schema>.<table>"` | `kSpec.write_table = "…"` — relation `INSERT`/`UPDATE`/`DELETE` target |
| `default_order` | primary key column name | `kSpec.default_order = "…"` |
| `max_limit` | `500` | `kSpec.max_limit = …` |
| `operation_filters.<op>` | `default_filters.<op>` (top-level manifest) | Filter string array on each route entry, e.g. `{"sgrn::datastore::filters::UserAuthFilter"}` |
| `default_filters.<op>` (top-level) | `UserAuthFilter` for every op | Fallback filter chain when a table has no `operation_filters` override for that op |

`default_filters` is manifest-only; it is **not** copied into the
`.gen.hpp`. Its effect is fully expanded into each route's filter list at
generation time.

#### Operations → HTTP routes and handler methods

| Manifest `operations` value | Route table | HTTP | Path | Handler member |
|---|---|---|---|---|
| `list` | `kRoutes` | `GET` | `/api/v1/<kebab-table>` | `handleList` |
| `create` | `kRoutes` | `POST` | `/api/v1/<kebab-table>` | `handleCreate` |
| `get` | `kItemRoutes` | `GET` | `/api/v1/<kebab-table>/{id}` | `handleGet` |
| `update` | `kItemRoutes` | `PATCH` | `/api/v1/<kebab-table>/{id}` | `handleUpdate` |
| `delete` | `kItemRoutes` | `DELETE` | `/api/v1/<kebab-table>/{id}` | `handleDelete` |

**Wire contract (explicit):** each row above is a Drogon route registered at
startup from the generated `kRoutes` / `kItemRoutes` arrays. Runtime behavior
(live in `CrudViewEngine.cpp`, not regenerated) is documented in
[`documentation/datastore/crud_views.md`](../../../../../documentation/datastore/crud_views.md).
Summary:

| Op | Client sends | Server returns |
|---|---|---|
| `list` | `Authorization` + optional query: `column=op.value` filters, `order`, `limit`, `offset` | `200` JSON **array** of rows (PK + tenant + whitelisted columns) |
| `get` | `Authorization`; path `{id}` = PK | `200` JSON object, or `404 NotFound` |
| `create` | `Authorization` + JSON body (insertable whitelist columns only) | `201` JSON object of inserted row; tenant from session |
| `update` | `Authorization` + JSON body (updatable fields only) + path `{id}` | `200` JSON object, or `404` / `400` if body empty |
| `delete` | `Authorization` + path `{id}` | `204 No Content` (always, even when zero rows matched) |

Filter query syntax: **`name=eq.acme`**, **`status=in.a,b`**, **`title=like.%25foo%25`**
(PostgREST-style `operator.value`; unsupported column/op → `400` scope `Query`).
Reserved query keys: `order`, `limit`, `offset` (not filters). Auth filter on
every route defaults to `UserAuthFilter` unless overridden per operation in
the manifest.

Example — manifest entry with `"operations": ["list", "get"]` only:

```cpp
static inline const std::array<RouteConfig, 1> kRoutes = {{
    {"/api/v1/domains", &DomainsView::handleList, {drogon::Get}, {"…UserAuthFilter"}},
}};
static inline const std::array<ItemRouteConfig, 1> kItemRoutes = {{
    {"/api/v1/domains/{id}", &DomainsView::handleGet, {drogon::Get}, {"…UserAuthFilter"}},
}};
```

#### Column-level keys → `Field` entries in `kFields`

Each non-excluded live column becomes one element of
`static constexpr std::array<Field, N> kFields`.

| Manifest `columns.<name>` key | Default when column omitted from `columns` | Default when column listed with empty `{}` | Generated `Field{…}` fragment |
|---|---|---|---|
| (presence) | column still emitted if it exists on the table | same | `"<name>", FieldType::<T>, …` |
| `filter_ops` | `[]` → not filterable | `[]` → not filterable | third arg: `0`, or `Op::Eq \| Op::Like`, etc. |
| `insertable` | `true` | `true` | fourth arg: `true` / `false` |
| `updatable` | `true` | `true` | fifth arg: `true` / `false` |

`FieldType::<T>` always comes from the **live column's** Postgres
`udt_name`, never from the manifest (see [Postgres type mapping](#postgres-type-mapping)).

Filter-op manifest strings map to `Op` bitmask bits:

| Manifest `filter_ops` string | C++ expression |
|---|---|
| `eq` | `Op::Eq` |
| `neq` | `Op::Neq` |
| `gt` | `Op::Gt` |
| `gte` | `Op::Gte` |
| `lt` | `Op::Lt` |
| `lte` | `Op::Lte` |
| `like` | `Op::Like` |
| `in` | `Op::In` |

Multiple ops are OR'd: `["eq", "like"]` → `Op::Eq | Op::Like`. A single
op is emitted as `static_cast<uint8_t>(Op::Eq)` because a bare `Op::X`
does not implicitly convert to the `uint8_t filter_ops` field.

#### Column defaults and `kFields` membership

A column appears in `kFields` **if and only if** all of the following hold:

- it exists on the live table;
- it is **not** the primary key;
- it is **not** the resolved tenant column;
- its name is **not** in `SENSITIVE_COLUMNS`
  (`password`, `token_secret_hash`, `password_hash`, `secret`).

Everything else on the table is included automatically — the manifest does
**not** need a per-column entry unless that column deviates from the
defaults. Omitting a column from `columns` is **not** the same as hiding
it: the column is still readable and writable; it is simply not
filterable (`filter_ops = 0`).

Concrete example from `core.users` (manifest lists six columns; eighteen
appear in `kFields`):

| Column | In manifest `columns`? | In `kFields`? | `filter_ops` in C++ | `insertable` / `updatable` |
|---|---|---|---|---|
| `id` | — | no (PK) | — | — |
| `organisation` | — | no (tenant) | — | — |
| `password` | — | no (`SENSITIVE_COLUMNS`) | — | — |
| `first_name` | yes: `["eq","like"]` | yes | `Op::Eq \| Op::Like` | `true`, `true` |
| `phone_number` | no | yes | `0` | `true`, `true` |
| `role` | yes: `["eq"]` | yes | `static_cast<uint8_t>(Op::Eq)` | `true`, `true` |

#### Tenant column resolution

Used for `kSpec.tenant_column` when the manifest omits `tenant_column`:

1. Explicit `tenant_column` string in the manifest (must exist on live table).
2. Else first live column named `organisation` or `organisation_id`.
3. Else first column with a foreign key to `core.organisations`.

If none match → hard error. Tables like `core.organisations` (tenant
root), `storage.*` (ownership via `user_id` / `automated_service_id`),
and keyless views must stay **out of the manifest** and get hand-written
handlers instead.

#### Postgres type mapping

Live `information_schema.columns.udt_name` → `FieldType` (also used for
`kSpec.pk`):

| Postgres `udt_name` | `FieldType` |
|---|---|
| `int2`, `int4` | `Int` |
| `int8` | `BigInt` |
| `text`, `varchar`, `bpchar`, `uuid`, `inet`, `numeric` | `Text` |
| `bool` | `Bool` |
| `timestamptz`, `timestamp`, `date` | `Timestamp` |
| `jsonb`, `json` | `Jsonb` |
| (anything else) | `Text` + generator warning |

Primary key and tenant column types are resolved the same way but those
columns are not repeated inside `kFields`.

#### End-to-end mapping example (`core.domains`)

Manifest fragment:

```json
"domains": {
  "tenant_column": "organisation",
  "read_relation": "core.domains",
  "write_table": "core.domains",
  "default_order": "id",
  "max_limit": 500,
  "operations": ["list", "get", "create", "update", "delete"],
  "columns": {
    "name": { "filter_ops": ["eq", "like"] }
  }
}
```

Generated C++ (abbreviated):

```cpp
class DomainsView : public CrudViewHandler<DomainsView> {
    static constexpr std::array<Field, 1> kFields = {{
        {"name", FieldType::Text, Op::Eq | Op::Like, true, true},
    }};
    static constexpr CrudViewSpec kSpec{
        .read_relation = "core.domains",
        .write_table = "core.domains",
        .tenant_column = "organisation",
        .pk = {"id", FieldType::Int},          // from live DB, not manifest
        .fields = kFields,
        .default_order = "id",
        .max_limit = 500,
    };
    // kRoutes: GET list + POST create on /api/v1/domains
    // kItemRoutes: GET/PATCH/DELETE on /api/v1/domains/{id}
};
```

Live columns `id`, `organisation`, and `name` exist; only `name` lands in
`kFields` because `id` is PK and `organisation` is tenant.

### How it works (summary)

1. **Load and validate `crud/manifest.json`** — unknown keys, bad filter
   references, and inconsistent `operation_filters` fail fast with manifest
   paths in the error text.
2. **Connect** via `psycopg2` and walk the manifest allow-list.
3. **Per table**, run the [exact algorithm](#generation-behavior-exact-algorithm)
   above: introspect structure from Postgres, merge intent from manifest,
   emit one `.gen.hpp`.
4. **Rewrite** `AllViews.gen.hpp`; **append-only** update of
   `RegisteredViews.cpp` (deleted registration lines stay deleted).

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

Semantics (see [Manifest table entry → C++ output](#manifest-table-entry--c-output-reference)
for the full key-by-key mapping):

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
  neither see nor override. For the full HTTP contract (filters, bodies,
  status codes), see
  [`documentation/datastore/crud_views.md`](../../../../../documentation/datastore/crud_views.md).

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

## Embedded VFS asset generators (`scripts/`)

These generators bake files (web UIs, SQL, configs, docs) into the
compiled binary as Zstd-compressed byte arrays in `.rodata`. At runtime
consumers look up assets by **virtual path** through a compile-time sorted
`AssetRegistry` — no filesystem dependency in production.

Unlike datastore DB codegen, VFS generation is **part of the normal build
graph**: `cmake --build` runs `bun vite build` (when needed) then
`generate_embedded_assets.py`. Output headers live under the **build
directory** (`<build>/…/dashboard_generated/`, `sql_generated/`, …), not
in the source tree.

### End-to-end pipeline (web UI example)

```
sgrn/web/datastore/src/**     React/TS sources
        │
        ▼  bun install + VITE_BASE_PATH=/datastore bun run build
sgrn/web/datastore/build/     Vite dist (index.html, assets/*.js, …)
        │
        ▼  generate_web_headers.py  (= generate_embedded_assets.py --kind web)
<build>/sgrn/lib/datastore/src/dashboard_generated/web_assets.hpp
        │
        ▼  #include <web_assets.hpp>  +  compile into sgrn_datastore
Binary serves GET /index.html, /assets/… from sgrn::datastore::assets::web::ASSETS[]
```

Gateway follows the same shape with `sgrn/web/gateway/` →
`sgrn::gateway::assets::web`.

### End-to-end pipeline (gateway SPA)

```
sgrn/web/gateway/src/**        Svelte/TS sources
        │
        ▼  bun run build  (touches dist/.build_done)
sgrn/web/gateway/dist/         Vite output (index.html, assets/*, …)
        │
        ▼  generate_web_headers.py  (--namespace sgrn::gateway::assets::web)
<build>/sgrn/lib/gateway/web_generated/web_assets.hpp
        │
        ▼  compiled into sgrn_gateway  +  #include <web_assets.hpp>
Crow registers GET for each virtual_path; SPA fallback + runtime index.html patch
```

Gateway does **not** set `VITE_BASE_PATH` in CMake (unlike the datastore
dashboard). Subpath deployment is handled at **response time** by injecting
`<base href="…">` and `window.__SGRN_BASE__` from nginx's
`X-Forwarded-Prefix` (see `gateway/adapters/http/assets.cpp`).

### Build directory map

All generated VFS headers live under the **CMake binary dir**, not in
`git`. Typical layout after `cmake --build <dir>`:

| Variable / path | Physical location (example) | Master header |
|---|---|---|
| `DASHBOARD_BUILD_DIR` | `sgrn/web/datastore/build/` (Vite dist, **in source tree**) | — |
| `DASHBOARD_GEN_DIR` | `<build>/sgrn/lib/datastore/src/dashboard_generated/` | `web_assets.hpp` |
| `SQL_GEN_DIR` | `<build>/sgrn/lib/datastore/src/sql_generated/` | `sql_assets.hpp` |
| `CONFIG_GEN_DIR` | `<build>/sgrn/lib/datastore/src/config_generated/` | `config_assets.hpp` |
| `WEB_DIST_DIR` | `sgrn/web/gateway/dist/` | — |
| `WEB_GEN_DIR` | `<build>/sgrn/lib/gateway/web_generated/` | `web_assets.hpp` |
| `DOC_GEN_DIR` | `<build>/sgrn/lib/gateway/doc_generated/` | `doc_assets.hpp` |

`sgrn_datastore` lists the three datastore master headers as **sources** and
adds the three `*_GEN_DIR` paths to `target_include_directories`, so
`#include <web_assets.hpp>` resolves without copying headers into
`sgrn/apps/datastore/`.

### Changing an embedded web UI (developer loop)

You normally **never** run the Python generators by hand — edit frontend
sources and rebuild:

```bash
# Repo root; micromamba activate SGRN if needed.

# Datastore React dashboard (also rebuilds when TS bindings change):
cmake --build <build-dir> --target sgrn_datastore
# or explicitly:
cmake --build <build-dir> --target sgrn_dashboard_assets

# Gateway Svelte SPA (pulled in by the gateway library target):
cmake --build <build-dir> --target sgrn_gateway
```

What invalidates each step (see `sgrn/lib/datastore/src/CMakeLists.txt` and
`sgrn/lib/gateway/CMakeLists.txt`):

| Trigger | Effect |
|---|---|
| Any file under `sgrn/web/*/src/` (CONFIGURE_DEPENDS glob) | Vite rebuild |
| `sgrn/typescript/{datastore,gateway,types}/` binding changes | Dashboard and/or gateway Vite rebuild |
| `package.json` / `bun.lock` (dashboard only: also binding `package.json`) | `bun install` stamp, then Vite |
| New/changed file in `dist/` or `build/` output | Zstd header regen on next build |
| Edit to `generate_embedded_assets.py` / `generate_web_headers.py` | All dependent custom commands rerun |

After a UI change, if the browser still serves old hashed JS/CSS, force a
full rebuild of the asset target — stale `web_assets.hpp` means the binary
still registers old `virtual_path` keys.

### Virtual paths, Vite base URL, and HTTP routes

The generator stores lookup keys in `EmbeddedAsset::virtual_path` with a
**leading slash** (`/index.html`, `/assets/index-abc123.js`), produced by
`virtual_prefix + "/" + relative_path` in the master header.

| Layer | Datastore dashboard | Gateway SPA |
|---|---|---|
| Vite `base` at build time | `VITE_BASE_PATH=/datastore` (CMake) | default `/` |
| Embedded route keys | `/index.html`, `/assets/…` | same shape under `/` |
| Public URL behind nginx | Often `https://host/datastore/…` | Often `https://host/gateway/…` |
| Server-side base fix | None (assets built with `/datastore` prefix in HTML) | Runtime patch on `index.html` only |

**Datastore (Drogon):** `registerDashboardAssets()` registers
`GET` on each `virtual_path` exactly as emitted. When the table contains
`/index.html`, an additional `GET /` handler aliases the same cached
response (`init/assets.hpp`).

**Gateway (Crow):** one dynamic route per asset; unknown non-API paths fall
back to patched `index.html`. API prefixes (`/data`, `/registry`, `/ws`, …)
never hit the SPA fallback (`assets.cpp`).

**Lookup API:** `VFS.find("/index.html")` uses exact `string_view` match
(compile-time sorted binary search). Paths without the leading slash will
not match.

### Datastore dashboard vs gateway SPA (web generators)

| | **Datastore** | **Gateway** |
|---|---|---|
| Sources | `sgrn/web/datastore/` (React) | `sgrn/web/gateway/` (Svelte) |
| Vite output dir | `sgrn/web/datastore/build/` | `sgrn/web/gateway/dist/` |
| TS bindings glob | `sgrn/typescript/datastore/` + `types/` | `sgrn/typescript/gateway/` |
| `bun install` in CMake | yes (stamp + deps on lockfiles) | no (build assumes deps present) |
| Generator namespace | `sgrn::datastore::assets::web` | `sgrn::gateway::assets::web` |
| HTTP stack | Drogon static handlers | Crow + SPA fallback |
| `index.html` over the wire | Zstd or plain like other assets | always decompressed + patched head |

Both call the same shim with `--level 22` and `--master web_assets.hpp`.

### `generate_embedded_assets.py` — unified generator

#### CLI reference

```bash
python3 scripts/generate_embedded_assets.py <src_dir> <out_dir> \
    --namespace sgrn::datastore::assets::web \
    --kind web \
    [--extensions .html .js .css …] \
    [--virtual-prefix /sql] \
    [--master web_assets.hpp] \
    [--level 19] \
    [--exclude compile_commands.json …]
```

| Flag | Default | Effect |
|---|---|---|
| `src_dir` | (required) | Root directory scanned recursively |
| `out_dir` | (required) | Where per-file and master headers are written |
| `--namespace` | `sgrn::datastore::assets::web` | C++ namespace wrapping every symbol |
| `--kind` | `web` | Semantic category → `AssetKind` enum value |
| `--extensions` | kind-specific set (see below) | Filter; omit filter when kind default is `None` (`other`) |
| `--virtual-prefix` | `""` | Prepended to every lookup key (`/sql` + `init.sql` → `/sql/init.sql`) |
| `--master` | `assets.hpp` | Aggregating header filename |
| `--level` | `19` | Zstd level (CMake uses `22`) |
| `--exclude` | none | Skip files whose relative path or basename contains a pattern |

Default extensions per `--kind`:

| `--kind` | Extensions scanned | Special processing |
|---|---|---|
| `web` | `.html .js .mjs .css .svg .png .jpg .jpeg .ico .webp .woff .woff2` | raw bytes |
| `sql` | `.sql` | **only** `<src_dir>/init.sql`; `\i` includes inlined recursively; comments stripped |
| `config` | `.conf .service` | `#` comments stripped; `.json` included when passed via `--extensions` |
| `cert` | `.pem .crt .key .cer` | raw bytes |
| `other` | all files | raw bytes |

`--kind` → C++ `AssetKind`:

| `--kind` | Generated `AssetKind` constant |
|---|---|
| `web` | `sgrn::AssetKind::Web` |
| `sql` | `sgrn::AssetKind::Sql` |
| `config` | `sgrn::AssetKind::Config` |
| `cert` | `sgrn::AssetKind::Cert` |
| `other` | `sgrn::AssetKind::Other` |

#### Generation behavior (exact algorithm)

1. Recursively scan `src_dir` for files matching `--extensions` (or all
   files when extensions default to `None`).
2. For `kind == sql`: process **only** `init.sql` at the root of
   `src_dir`; every other `.sql` file is pulled in via `\i`/`\ir` inside
   `flattenSql()`.
3. Apply `--exclude` patterns (substring match on relative path or basename).
4. For each kept file:
   - **Preprocess** payload (`flattenSql` / `stripConfigComments` / raw read).
   - **Compress** with Zstd at `--level`.
   - **Emit** one per-file header `<rel_path_with_slashes_as_underscores>.hpp`.
5. **Emit master header** (`--master`): `#include` every per-file header,
   define `ASSETS[]`, `VFS = AssetRegistry(ASSETS)`, `ASSET_COUNT`.

Per-file headers are **rewritten every run** (full regen). There is no
append-only mode unlike `RegisteredViews.cpp`.

#### Source file → C++ VFS output (reference)

##### Path and symbol naming

| Source file (relative to `src_dir`) | Per-file header | C++ symbol prefix | Virtual path (lookup key) |
|---|---|---|---|
| `index.html` | `index.html.hpp` | `INDEX_HTML_DATA`, `_COMPRESSED_SIZE`, `_ORIGINAL_SIZE` | `index.html` (or `{virtual-prefix}/index.html`) |
| `assets/app.js` | `assets_app.js.hpp` | `ASSETS_APP_JS_*` | `assets/app.js` |
| `postgres/init.sql` (sql kind) | `init.sql.hpp` | `INIT_SQL_*` | `init.sql` |

Symbol rule: relative path → `SCREAMING_SNAKE_CASE`
(`assets/app.js` → `ASSETS_APP_JS`). Header filename rule: `/` and `\`
→ `_`.

MIME type comes from file extension via `MIME_MAP` (web/config/json);
unknown extensions get `application/octet-stream`.

Full `MIME_MAP` (extension → `content_type` string in `ASSETS[]`):

| Extension | MIME |
|---|---|
| `.html` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js`, `.mjs` | `application/javascript; charset=utf-8` |
| `.svg` | `image/svg+xml` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.ico` | `image/x-icon` |
| `.webp` | `image/webp` |
| `.woff` | `font/woff` |
| `.woff2` | `font/woff2` |
| `.json` | `application/json` |
| `.txt`, `.sql`, `.toml` | `text/plain; charset=utf-8` |
| (other) | `application/octet-stream` |

##### Per-file header shape

Each source file becomes a header like:

```cpp
// Auto-generated — do not edit. Source: assets/app.js
#pragma once
#include <cstdint>
#include <cstddef>

namespace sgrn::datastore::assets::web {

inline constexpr uint8_t ASSETS_APP_JS_DATA[] = {
    0x28, 0xB5, 0x2F, 0xFD, …   // Zstd-compressed payload in .rodata
};
inline constexpr size_t ASSETS_APP_JS_COMPRESSED_SIZE = sizeof(ASSETS_APP_JS_DATA);
inline constexpr size_t ASSETS_APP_JS_ORIGINAL_SIZE   = 12345;

} // namespace …
```

##### Master header shape (`web_assets.hpp`, `sql_assets.hpp`, …)

```cpp
#pragma once
#include <sgrn/assets/EmbeddedAsset.hpp>
#include "index.html.hpp"
#include "assets_app.js.hpp"
// …

namespace sgrn::datastore::assets::web {

inline constexpr sgrn::EmbeddedAsset ASSETS[] = {
    {"index.html", "text/html; charset=utf-8",
      INDEX_HTML_DATA, INDEX_HTML_COMPRESSED_SIZE, INDEX_HTML_ORIGINAL_SIZE,
      sgrn::AssetKind::Web},
    {"assets/app.js", "application/javascript; charset=utf-8",
      ASSETS_APP_JS_DATA, …, sgrn::AssetKind::Web},
};

inline constexpr auto VFS = sgrn::AssetRegistry(ASSETS);
inline constexpr size_t ASSET_COUNT = VFS.size();

} // namespace …
```

Runtime lookup: `sgrn::datastore::assets::web::VFS.find("/index.html")`
→ `const EmbeddedAsset*` or `nullptr`. Iteration in declaration order:
`for (i = 0; i < ASSET_COUNT; ++i) ASSETS[i]`.

##### SQL kind — `\i` inlining and comment stripping

Only `postgres/init.sql` is passed to the generator. The script:

1. Walks each line; `\i path` / `\ir path` recursively inlines the
   referenced file (relative to the including file's directory).
2. Strips `/* */` block comments and `--` line comments.
3. Compresses the flattened script into a **single** `ASSETS[0]` entry.

At runtime `decompressSqlAssets()` in `bootstrap.hpp` decompresses
`sql::ASSETS[0]` and feeds the result to Postgres during `--init-db`.

##### Config kind — what gets embedded

CMake invocation (`sgrn/lib/datastore/src/CMakeLists.txt`):

- Source: `sgrn/lib/datastore/configs/`
- Extensions: `.json .conf .service`
- Excludes: `compile_commands.json`, `endpoints.json`, nginx snippets
  that are templates only (`fastcgi*.conf`, `snakeoil.conf`)
- Namespace: `sgrn::datastore::assets::config`

`extractConfigAssets()` writes each asset to disk under the operation
directory, applying path templates (`{{OPERATION_DIR}}`, etc.) to the
decompressed text.

### CMake targets and build integration

| Consumer | Frontend / source dir | Build step | Generator | Output (under build dir) | CMake target |
|---|---|---|---|---|---|
| **Datastore dashboard** | `sgrn/web/datastore/` | `bun install` + `VITE_BASE_PATH=/datastore bun run build` | `generate_web_headers.py` | `dashboard_generated/web_assets.hpp` | `sgrn_dashboard_assets` |
| **Datastore SQL** | `sgrn/lib/datastore/postgres/` | none | `generate_embedded_assets.py --kind sql` | `sql_generated/sql_assets.hpp` | `sgrn_sql_assets` |
| **Datastore config** | `sgrn/lib/datastore/configs/` | none | `generate_embedded_assets.py --kind config` | `config_generated/config_assets.hpp` | `sgrn_config_assets` |
| **Gateway SPA** | `sgrn/web/gateway/` | `bun run build` → `dist/` | `generate_web_headers.py` | `web_generated/web_assets.hpp` | (dependency of `sgrn_gateway`) |
| **Gateway docs** | `documentation/gateway/` | none | `generate_embedded_assets.py --kind other` | `doc_generated/doc_assets.hpp` | (dependency of `sgrn_gateway`) |

Generated header paths are exported as `CACHE INTERNAL` variables
(`DASHBOARD_GEN_DIR`, `SQL_GEN_DIR`, …) so `sgrn/apps/datastore` can
`#include <web_assets.hpp>` via the build's include path.

**Dependency tracking:** SQL/config/doc custom commands use
`file(GLOB_RECURSE … CONFIGURE_DEPENDS)` on source trees. Dashboard/gateway
web builds also depend on shared TypeScript bindings under
`sgrn/typescript/{datastore,gateway}/` so API type changes invalidate the
frontend bundle.

### Runtime consumption (generated C++ → behavior)

| Namespace | Included from | Used for |
|---|---|---|
| `sgrn::datastore::assets::web` | `init/assets.hpp` | `registerDashboardAssets()` — Drogon routes at each `virtual_path`; `/` aliases `index.html` |
| `sgrn::datastore::assets::sql` | `bootstrap/bootstrap.hpp` | `--init-db`: decompress single flattened `init.sql`, execute against Postgres |
| `sgrn::datastore::assets::config` | `bootstrap/bootstrap.hpp` | `init` / bootstrap: extract nginx, systemd, `sgrn.json`, … to operation dir |
| `sgrn::gateway::assets::web` | `gateway/adapters/http/assets.cpp` | Crow dynamic routes; SPA fallback; runtime `<!-- SGRN_RUNTIME_HEAD -->` patch on `index.html` for nginx `X-Forwarded-Prefix` |
| `sgrn::gateway::assets::doc` | (compiled in; no handler yet) | Embedded man-page style docs under `documentation/gateway/` |

**HTTP serving pattern (both web bundles):**

- Clients advertising `Accept-Encoding: zstd` receive the pre-compressed
  blob directly (except gateway `index.html`, which is always decompressed
  so runtime `<base>` / `window.__SGRN_BASE__` injection can run).
- Other clients trigger one-time Zstd decompression; responses are cached
  (`CachedAssetResponses` in datastore, `std::once_flag` + map in gateway).

### `generate_web_headers.py` — compatibility shim

Legacy CMake rules call this script instead of `generate_embedded_assets.py`
directly. It forwards to:

```text
generate_embedded_assets.py <src> <out>
  --namespace <ns>  --kind web  --master web_assets.hpp  --level <n>
```

The `--use-bun` flag is accepted but ignored (the frontend build is always
a separate CMake step). Prefer calling `generate_embedded_assets.py`
directly for new asset categories.

### `embded_web.py` — legacy manual embedder

Pre-dates the unified VFS system (filename is a historical typo; the module
docstring says `embed_web.py`). CLI:

```bash
python3 scripts/embded_web.py file1.html file2.js [-o web.hpp] [--level 19]
```

Behavior:

- Input is an **explicit file list**, not a directory walk.
- Virtual paths are **`/` + basename only** (`dashboard.html` → `"/dashboard.html"`), not nested dist paths.
- Emits one monolithic header: `WebAsset` struct array + `WEB_ASSETS[]` /
  `WEB_ASSET_COUNT` — no `EmbeddedAsset`, no `AssetRegistry`, no per-file
  headers.
- Symbol names derive from **filename** only (`app.min.js` → `APP_MIN_JS`).

Useful for quick one-off experiments; **not wired into CMake**. New work
should use `generate_embedded_assets.py`.

### Adding a new embedded asset category

Pattern used by SQL, config, gateway docs, and both web UIs:

1. Choose `--kind`, `--namespace` (`sgrn::{project}::assets::{kind}`), and
   `--master` filename (`*_assets.hpp`).
2. Add a `add_custom_command(OUTPUT …)` in the relevant `CMakeLists.txt`
   that invokes `generate_embedded_assets.py` (or the web shim for `--kind
   web`).
3. `file(GLOB_RECURSE … CONFIGURE_DEPENDS)` on the source tree for
   dependency tracking.
4. List the master header in a library/executable target and add
   `target_include_directories(… PRIVATE "${GEN_DIR}")`.
5. Consume via `#include <…_assets.hpp>` and `namespace::VFS.find(…)`.

Keep generation **out of** the default DB-codegen loop — VFS headers are
build products, not git-tracked sources.

### Manual regeneration (outside CMake)

```bash
# Repo root. Run only when debugging the generator itself.

# Datastore dashboard dist → headers (after a manual Vite build)
cd sgrn/web/datastore && VITE_BASE_PATH=/datastore bun run build
python3 scripts/generate_web_headers.py \
    sgrn/web/datastore/build \
    /tmp/dashboard_generated \
    --namespace sgrn::datastore::assets::web --level 22

# Gateway dist → headers
cd sgrn/web/gateway && bun run build
python3 scripts/generate_web_headers.py \
    sgrn/web/gateway/dist \
    /tmp/web_generated \
    --namespace sgrn::gateway::assets::web --level 22

# SQL bundle only
python3 scripts/generate_embedded_assets.py \
    sgrn/lib/datastore/postgres /tmp/sql_generated \
    --kind sql --master sql_assets.hpp \
    --namespace sgrn::datastore::assets::sql --level 22
```

Normally you just `cmake --build <dir>` and let the custom commands run.

### VFS troubleshooting

- **`zstandard` import error** → `pip install zstandard` (or use the SGRN
  conda env).
- **`bun: command not found`** during dashboard/gateway build → install
  [Bun](https://bun.sh); web asset compression runs only after Vite succeeds.
- **Empty `ASSET_COUNT`** → wrong `src_dir`, extension filter too narrow,
  or (for SQL) missing `init.sql` at the postgres root.
- **Dashboard 404 on assets** → rebuild `sgrn_dashboard_assets`; stale
  `web_assets.hpp` if the Vite hash filenames changed but CMake did not rerun.
- **Gateway SPA wrong base URL behind nginx** → gateway patches
  `index.html` using `X-Forwarded-Prefix`; check nginx sends the header
  (see `sites-enabled/sgrn.conf`).

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

### Embedded VFS (`generate_embedded_assets.py`)

Minimal port:

1. Copy `scripts/generate_embedded_assets.py` (depends on `pip install
   zstandard` only).
2. Copy `sgrn/lib/core/include/sgrn/assets/EmbeddedAsset.hpp` and wire your
   decompressor (SGRN uses `sgrn::utils::compression::decompressStringZstd`).
3. Pick `--namespace` / `--kind` / `--master` and add one CMake custom
   command — mirror `sgrn_dashboard_assets` or `sgrn_sql_assets`.
4. At runtime, iterate `ASSETS[]` or `VFS.find("/your/path")`; register
   HTTP routes from `virtual_path` + `content_type` if serving web assets.

Fork knobs: `MIME_MAP`, `default_extensions`, `flattenSql()` /
`stripConfigComments()` behavior, and Zstd `--level`. No database or Drogon
dependency in the generator itself.

### What NOT to take

- `postgres/` — the SGRN schema, roles, and seeds. Bring your own tables;
  only the *manifest convention* (`crud/manifest.json` allow-list shape)
  is worth copying verbatim.
- `StorageApiHandler`, the metadata endpoints, `postgrest.*` leftovers —
  SGRN-specific consumers, not part of the generator runtime.
- The dashboard/TS bindings — they speak the HTTP grammar, so they keep
  working against any fork's routes, but they are not required by it.
