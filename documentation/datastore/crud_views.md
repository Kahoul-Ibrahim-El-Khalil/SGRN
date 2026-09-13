# Generated CRUD views (HTTP contract)

These endpoints are **compile-time generated** from `crud/manifest.json` +
the live Postgres schema (`generate_views.py`). Each exposed table becomes a
Drogon handler class (`*View.gen.hpp`) that delegates to the shared
`CrudViewEngine`.

**Related docs**

- Generator mapping (manifest → C++): [`sgrn/lib/datastore/scripts/generators/README.md`](../../sgrn/lib/datastore/scripts/generators/README.md) — section `generate_views.py`
- Other HTTP groups: [README.md](README.md) (auth, storage, admin, …)

Base path: `/api/v1/`

---

## Authentication and tenancy

Every generated view route listed in the current manifest uses
`UserAuthFilter` on all operations (see `default_filters` in
`crud/manifest.json`).

| Requirement | Value |
|---|---|
| Header | `Authorization: Bearer <session-token>` |
| Session store | Redis (validated before the handler runs) |
| Tenant source | `session_json["user"]["organisation"]` (string) |
| Tenant in SQL | Bound server-side as `$1` on every query — **never** accepted from the client |

If the filter passes but the session has no organisation string, the handler
returns `401` with scope `Authentication`:

```json
{ "error": "Missing organisation identity in session", "scope": "Authentication" }
```

The tenant column itself (`organisation` on most `core.*` tables) is **not**
in the public field whitelist: clients cannot filter on it, set it in POST/PATCH
bodies, or receive it as a writable column. It is included in **read**
responses because `selectList()` always returns PK + tenant + whitelisted fields.

---

## Route naming

| Manifest table | HTTP path prefix |
|---|---|
| `core.domains` | `/api/v1/domains` |
| `core.users` | `/api/v1/users` |
| `core.automated_services` | `/api/v1/automated-services` |
| `core.user_domain_permissions` | `/api/v1/user-domain-permissions` |

Rule: schema prefix is dropped; table name is kebab-cased (`_` → `-`).

Item routes append `/{id}` where `{id}` is the primary-key value as a path
segment (string on the wire; bound to the PK column type in SQL).

---

## Operations (request / response contract)

All successful JSON bodies use `Content-Type: application/json`.
Errors use the shape documented in [Error responses](#error-responses).

### `list` — `GET /api/v1/<resource>`

Returns a **JSON array** of objects (not wrapped in `{ "data": … }`).

| Aspect | Contract |
|---|---|
| Query filters | See [Filter query parameters](#filter-query-parameters) |
| Pagination | `limit` (integer, capped by view `max_limit`, default 500), `offset` (integer, default 0) |
| Sort | `order=<column>` ascending, or `order=<column>.desc` descending |
| Default sort column | View `default_order` (usually PK name, e.g. `id`) when `order` is omitted |
| Sortable columns | Only columns in the view whitelist (`kFields`) — **not** PK or tenant unless they appear there |
| Response columns | PK + tenant column + every whitelisted field |
| Success | `200 OK`, body `[{ … }, …]` |
| Empty result | `200 OK`, body `[]` |

**Example** — list domains whose name contains `prod`:

```http
GET /api/v1/domains?name=like.%25prod%25&order=name&limit=50
Authorization: Bearer <token>
```

```json
[
  { "id": 3, "organisation": "acme", "name": "prod-east" },
  { "id": 7, "organisation": "acme", "name": "prod-west" }
]
```

### `get` — `GET /api/v1/<resource>/{id}`

| Aspect | Contract |
|---|---|
| Path `{id}` | Primary key value |
| Query params | Ignored |
| Success | `200 OK`, single JSON object |
| Not found (wrong id or other tenant) | `404`, `{ "error": "No matching row", "scope": "NotFound" }` |

### `create` — `POST /api/v1/<resource>`

| Aspect | Contract |
|---|---|
| Body | JSON object; `Content-Type: application/json` required |
| Writable keys | Only whitelisted columns with `insertable: true` in the manifest |
| Ignored keys | PK, tenant column, non-insertable columns, unknown keys (silently skipped) |
| Tenant | Set from session — body cannot override |
| Success | `201 Created`, body is the inserted row (`RETURNING` same column set as reads) |
| Invalid JSON | `400`, scope `Body` |
| Type mismatch | `400`, scope `Body`, e.g. `"Field 'role' expects text but got boolean"` |

**Example** — create a domain:

```http
POST /api/v1/domains
Authorization: Bearer <token>
Content-Type: application/json

{ "name": "lab-01" }
```

```json
{ "id": 42, "organisation": "acme", "name": "lab-01" }
```

### `update` — `PATCH /api/v1/<resource>/{id}`

Partial update: only keys present in the body are considered.

| Aspect | Contract |
|---|---|
| Body | JSON object (at least one updatable field required) |
| Writable keys | Whitelisted columns with `updatable: true` |
| Tenant / PK | Never updatable |
| Success | `200 OK`, body is the updated row |
| Empty or non-updatable body | `400`, `"No updatable fields supplied"`, scope `Body` |
| Not found | `404`, scope `NotFound` |

`PUT` is **not** registered — only `PATCH`.

### `delete` — `DELETE /api/v1/<resource>/{id}`

| Aspect | Contract |
|---|---|
| Body | None |
| Success | `204 No Content` (empty body) |
| Not found | Still `204` — delete is idempotent; zero rows deleted does not change the status |

---

## Filter query parameters

Syntax follows a PostgREST-style **`column=operator.value`** pattern (one
query parameter per filter).

| Operator | Query example | SQL |
|---|---|---|
| `eq` | `role=eq.admin` | `role = $n` |
| `neq` | `status=neq.disabled` | `status != $n` |
| `gt` | `id=gt.10` | `id > $n` |
| `gte` | `id=gte.10` | `id >= $n` |
| `lt` | `id=lt.100` | `id < $n` |
| `lte` | `id=lte.100` | `id <= $n` |
| `like` | `name=like.%25acme%25` | `name LIKE $n` (client supplies `%` wildcards) |
| `in` | `status=in.active,pending` | `status IN ($n, $m, …)` (comma-separated, no spaces required) |

Rules:

- The column must exist in the view whitelist **and** have that operator enabled
  in `crud/manifest.json` (`filter_ops`). Otherwise → `400`, scope `Query`,
  message `"Unsupported filter: <column>"`.
- Malformed operator prefix (missing `.`, unknown op) → same `400`.
- Empty `in` list → `400`.
- Reserved parameter names (not filters): `order`, `limit`, `offset`.

Pagination parameters are plain integers (not `operator.value`):

| Param | Behavior |
|---|---|
| `limit=N` | `N` clamped to `max_limit` from manifest (default 500). Invalid values ignored → default used. |
| `offset=N` | SQL `OFFSET N`. Invalid values ignored → no offset. |

Sort parameter:

| Form | Meaning |
|---|---|
| `order=email` | `ORDER BY email ASC` |
| `order=email.desc` | `ORDER BY email DESC` |
| (omitted) | `ORDER BY <default_order> ASC` |

---

## JSON field types (wire format)

Types come from Postgres (`udt_name`) at generation time.

| `FieldType` | JSON in responses | Accepted in POST/PATCH body |
|---|---|---|
| `Int` | number | integer, integral number, decimal string, or `0x` hex string |
| `BigInt` | **string** (decimal digits) | same as Int (64-bit range) |
| `Text` | string | string only |
| `Bool` | boolean | boolean only |
| `Timestamp` | string (ISO timestamp from Postgres) | non-empty string only (no epoch numbers) |
| `Jsonb` | parsed JSON value, or string if corrupt | any JSON value (re-serialized to jsonb) |

Null column values serialize as JSON `null`.

---

## Column visibility rules

What the generator puts in `kFields` (and therefore what clients can
filter/write) vs what appears in responses:

| Column kind | In `kFields`? | In GET/POST response? | Client can set on create/update? |
|---|---|---|---|
| Primary key | No | Yes (`id`, …) | No (DB-generated on create) |
| Tenant (`organisation`, …) | No | Yes | No (from session) |
| `SENSITIVE_COLUMNS` (`password`, …) | No | **No** | No |
| Normal table column | Yes (defaults) | Yes | Yes unless manifest sets `insertable`/`updatable: false` |

Manifest `columns` entries only **override defaults** (filter ops, insert/update
flags). Omitting a column from the manifest does **not** hide it from reads.

---

## Currently exposed views

From `crud/manifest.json` (regenerate after manifest changes):

| Resource | Operations | Notable column policy |
|---|---|---|
| `/api/v1/domains` | list, get, create, update, delete | `name`: filter `eq`, `like` |
| `/api/v1/users` | all five | `first_name`, `family_name`, `email`: `eq`, `like`; `role`, `domain`, `status`: `eq` (status also `in`) |
| `/api/v1/automated-services` | all five | `token`: not insertable/updatable; `name`: `eq`, `like`; `status`: `eq`, `in` |
| `/api/v1/user-domain-permissions` | all five | `user_id`: `eq`, `in`; `domain`: `eq` |

**Not generated:** `core.sessions` (`operations: []` — session lifecycle is
hand-written under `/api/v1/auth/…`).

Tables without manifest entries (e.g. `core.organisations`, all of `storage.*`)
use hand-written handlers instead — see [storage.md](storage.md) and
[admin.md](admin.md).

---

## End-to-end example: `core.domains`

### Postgres table

```sql
create table core.domains (
  id int generated always as identity primary key,
  organisation text not null references core.organisations (name),
  name text not null,
  unique (organisation, name)
);
```

### Manifest snippet

```json
"domains": {
  "tenant_column": "organisation",
  "operations": ["list", "get", "create", "update", "delete"],
  "columns": {
    "name": { "filter_ops": ["eq", "like"] }
  }
}
```

### Generated routes (abbreviated)

```cpp
GET    /api/v1/domains           → handleList
POST   /api/v1/domains           → handleCreate
GET    /api/v1/domains/{id}      → handleGet
PATCH  /api/v1/domains/{id}      → handleUpdate
DELETE /api/v1/domains/{id}      → handleDelete
```

Each route runs `UserAuthFilter` then `CrudViewEngine` with tenant =
session organisation.

---

## Error responses

Most handler errors:

```json
{
  "error": "<human-readable message>",
  "scope": "<category>"
}
```

Common scopes:

| Scope | Typical HTTP status | When |
|---|---|---|
| `Authentication` | 401 | Missing/invalid token; missing organisation in session |
| `Query` | 400 | Bad filter, sort column, or operator |
| `Body` | 400 | Invalid JSON, wrong types, no updatable fields |
| `NotFound` | 404 | GET/PATCH target row missing for this tenant |
| `Database` | 500 | SQL/driver failure |

Filter failures (before the handler) use the same JSON shape with scope
`Authentication` or `503` when Redis is unavailable.

---

## What is intentionally unsupported

- Composite primary keys
- Client-supplied tenant column
- `PUT` (use `PATCH`)
- Filtering or sorting on PK/tenant unless they appear in `kFields`
- Silent ignore of bad filters (always `400`)
- Tables without a detectable tenant column (must stay off manifest or get a
  hand-written handler)
- Runtime interpretation of `crud/manifest.json` (must regenerate C++ and rebuild)

---

## Changing the HTTP surface

1. Edit `crud/manifest.json` (operations, filters, column flags).
2. Apply schema migrations if columns/tables changed.
3. `sgrn_datastore --init-db`
4. `python3 sgrn/lib/datastore/scripts/generators/generate_views.py`
5. Review `git diff` on `src/handlers/generated/*.gen.hpp`
6. Rebuild — no live DB needed for compile

For the full developer loop including **frontend** and **embedded web
assets**, see [development.md](development.md). Generator internals:
[`generators/README.md`](../../sgrn/lib/datastore/scripts/generators/README.md).

---

## Out of scope (use hand-written handlers instead)

Generated CRUD is single-relation only. Do **not** use the manifest for:

- Multi-table **joins** or nested includes in one response
- Aggregations, window functions, or reporting queries
- `storage.*` tables (ownership via `user_id`, not `organisation`)
- Registration flows, password changes, token rotation
- File upload, streaming, or MinIO-backed objects

See [development.md](development.md) for the decision table and handler examples.
