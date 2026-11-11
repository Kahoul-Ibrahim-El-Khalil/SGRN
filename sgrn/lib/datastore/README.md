# 📦 SGRN Datastore

The **SGRN Datastore** is the high-throughput persistence, storage management, and orchestration engine of the SGRN suite. It serves as a resilient, multi-tenant middle layer between edge nodes (Gateways), client tools, and enterprise cloud/object storage backends.

---

## 🏛️ Core Architecture

The Datastore is engineered for maximum throughput, state temporal querying, and high availability.

### 1. Ingestion Pipeline

- **Batch Processing:** Pushes telemetry payloads in high-density batches (JSONL / binary formats), minimizing HTTP connection overhead.
- **WAL Decoupling:** Ingested streams are appended to a high-speed Write-Ahead Log (WAL) or SQLite buffer before async flushing, decoupling network ingest latency from database storage.

### 2. Anchor-Delta Persistence Model

- **Anchors:** Complete state snapshots of digital twin memory spaces.
- **Deltas:** Sparse payloads recording only modified memory offsets.
- **Timeline Reconstruction:** Replays Deltas over the nearest Anchor to satisfy temporal queries (_e.g._, "What was the exact memory map of DB2 at timestamp T?") with minimal storage overhead.

### 3. Forwarding & Storage Orchestration

- **PostgreSQL:** Dynamic SQL persistence for analytical query models.
- **Garage / S3 Object Storage:** Cold storage and immutable Parquet/JSONL block archiving.
- **Resilience & Queueing:** Automatic local buffer fallback during backend outages with smooth recovery queue draining.

---

## 🌐 HTTP REST API Reference

All endpoints are hosted under `/api/v1/`.

| Endpoint                           | Method   | Scope  | Description                                                          |
| :--------------------------------- | :------- | :----- | :------------------------------------------------------------------- |
| `/api/v1/auth/signin`              | `POST`   | Public | Authenticate user credentials and obtain session token               |
| `/api/v1/auth/signout`             | `POST`   | User   | Terminate current user session                                       |
| `/api/v1/storage/list`             | `GET`    | User   | List directory contents (virtual Drive space)                        |
| `/api/v1/storage/upload`           | `POST`   | User   | Direct single-file upload (files < 5MB)                              |
| `/api/v1/storage/upload/batch`     | `POST`   | User   | Multi-file and directory structure batch upload                      |
| **Resumable Uploads**              |          |        |                                                                      |
| `/api/v1/storage/upload/init`      | `POST`   | User   | Initiate a resumable chunked upload session                          |
| `/api/v1/storage/upload/chunk`     | `PUT`    | User   | Upload a binary chunk (`upload_id`, `chunk_index`)                   |
| `/api/v1/storage/upload/status`    | `GET`    | User   | Query upload progress & uploaded chunk indices                       |
| `/api/v1/storage/upload/complete`  | `POST`   | User   | Finalize and assemble uploaded chunks into storage object            |
| `/api/v1/storage/upload/abort`     | `DELETE` | User   | Cancel session and clean up temporary chunks                         |
| **Resumable Downloads**            |          |        |                                                                      |
| `/api/v1/storage/download`         | `GET`    | User   | Standard & HTTP Range (resumable) object download                    |
| **Admin & Telemetry**              |          |        |                                                                      |
| `/api/v1/admin/metaprobe/sessions` | `GET`    | Admin  | Query active user and service authentication sessions                |
| `/api/v1/admin/webhooks`           | `GET`    | Admin  | List registered event webhooks                                       |
| `/api/v1/admin/webhooks`           | `POST`   | Admin  | Register new webhook for auth events (`user.signin`, `user.signout`) |
| `/api/v1/admin/webhooks/{id}`      | `DELETE` | Admin  | Remove registered webhook endpoint                                   |

---

## 💻 Datastore Shell CLI & AngelScript REPL

The `sgrn_datastore_shell` CLI provides full administrative and storage management capabilities with built-in AngelScript automation.

### Canonical storage names

Storage authorities use this convention:

- `/` separates storage paths.
- `.` separates domain levels in a canonical authority.
- Domain labels are lower-case only: `production.line1` is valid, while
  `Production.Line1` is rejected.
- Organisation-domain files use
  `/file/<path>/<name>@<organisation>-<domain>[.<subdomain>...]`.
- User-owned files use
  `/file/<path>/<name>@<organisation>-<user>@<mail-domain>`.
  The user is resolved inside the authenticated organisation; the prefix is
  an authority label, not a client-side tenant override.

The shell translates these names to the authenticated API namespaces. The
explicit `/domains/...` and `/users/...` paths remain valid for compatibility.

An account may be organisation-bound (`domain = NULL`) or assigned to one
lower-case operational domain. Domain-assigned users are constrained by that
domain's permissions and allowed subpath. Changing the authority text cannot
select another organisation.

Uploads requested with `--encrypt` use the resumable AES-256-GCM pipeline and
SQLite key store. Domain policies still apply: `none` rejects encryption
requests, `optional` permits them, and `required` mandates them.

### Available Commands

| Command            | Arguments                                       | Description                                                       |
| :----------------- | :---------------------------------------------- | :---------------------------------------------------------------- |
| `rput`             | `<LOCAL> [REMOTE] [--chunk-size B] [--encrypt]` | Resumable chunked upload; `--encrypt` enables AES-256-GCM at rest |
| `upload-status`    | `<UPLOAD-ID>`                                   | Inspect status and uploaded chunk map of a resumable upload       |
| `upload-abort`     | `<UPLOAD-ID>`                                   | Abort an ongoing upload session                                   |
| `sessions`         | —                                               | Display active user sessions table (admin)                        |
| `webhooks`         | —                                               | List all registered event webhooks (admin)                        |
| `webhook-add`      | `<URL> [--secret S]`                            | Register a new webhook endpoint (admin)                           |
| `webhook-del`      | `<ID>`                                          | Remove a webhook endpoint by ID (admin)                           |
| `ls`, `cd`, `pwd`  | `[PATH]`                                        | Navigate virtual storage hierarchy                                |
| `get`, `put`       | `<FILE>`                                        | Download / upload single files                                    |
| `mkdir`, `rm`      | `<PATH>`                                        | Manage directories and objects                                    |
| `whoami`, `logout` | —                                               | Inspect session identity or sign out                              |

### AngelScript REPL Automation

All storage operations can be scripted via the embedded AngelScript engine:

```cpp
// AngelScript snippet for automated resumable upload & webhook setup
string uploadId = rput("large_archive.tar.gz", "/backups/large_archive.tar.gz", 5242880);
Print("Upload completed: " + uploadId);

string hookId = webhookAdd("https://api.example.com/events", "secret_token");
Print("Webhook registered with ID: " + hookId);
```

---

## 🖥️ Embedded Web Dashboard

The Datastore bundles a modern React dashboard (Vite + TailwindCSS):

- **Drive Workspace:** Interactive file explorer with drag-and-drop batch upload, range-resumable downloads, and real-time floating progress bars.
- **Admin Metaprobe:** Live user session probe, audit logs (including hash-chain verification via `GET .../audit-logs/verify`), and webhook subscriptions.
- **Analytics & Health:** Ingest rate monitoring, storage orphan detection, and quota enforcement.
- **Quotas Ledger:** Edit storage / entry / rate caps per user, service, organisation **or domain** (namespace caps cover the whole sub-tree). Columns that do not apply to the selected kind are hidden and never sent.
- **Domain Registry:** Create domains, rename (`POST .../domains/{id}/rename`) and re-parent (`POST .../domains/{id}/move` — including back to root) them from the registry rows; renames re-label every authority under the sub-tree.
- **Service Inventory:** Register services (with an optional initial storage cap), then edit status, metadata JSON and quota caps inline; rotate keys or soft-delete per row.
- **User Configuration:** Register users, then edit identity, assigned domain, lifecycle status (through the admin status route — suspending revokes sessions), personal-namespace permissions; reset passwords (revokes sessions) or soft-delete per row.
- **Organisation Management** (platform admins only): create organisations with an optional bootstrapped initial admin, then edit description, default domain and lifecycle status.
- **Roles & Permissions:** Grant/revoke domain permissions; view the current assignment matrix; create, edit (description / permission list), delete and assign/revoke dynamic RBAC roles to users or services (self-assignment refused server-side).
- **Session Control:** The live session probe carries a per-row **REVOKE** — terminates one session immediately (reason `admin_revoked`) across every instance, audited as `session.revoked`.
- **Organisation Management** (platform admins only): create organisations with an optional bootstrapped initial admin, then edit description, default domain and lifecycle status.

> **F12 note:** earlier dashboard builds silently rendered empty quota / organisation panels because the frontend still parsed the pre-unified quota response (`data.users` / `data.automated_services` / `data.organisation`). The frontend now consumes the unified `{kind, rows, count}` shape and the admin page exposes the Quotas, Domains and Organisations panels directly. The dashboard ships as a single-file bundle inside the binary — rebuild (`cmake --build .build/linux-static-release/ --target install`) and restart the `SGRN-datastore` service to serve the new panels.

---

## 🛡️ Admin REST API Reference

All admin routes require an authenticated session. Two admin levels gate
mutations:

- **Org admin** (`OrgAdminFilter`) — full control **inside the authenticated
  organisation**; cross-organisation targeting is rejected server-side.
- **Platform admin** (`PlatformAdminFilter`) — fleet-wide; the only level that
  may create or transition organisations, purge audit logs / orphans, change
  system configuration, or target another tenant (e.g.
  `PUT /api/v1/admin/quotas/organisation?name=OtherOrg`).

Domain admins resolve to org admins on hand-written lifecycle routes; on the
generated CRUD surface they are constrained by their assigned domain and
sub-path.

### Endpoints

| Endpoint                                         | Method(s)           | Minimum level              | Purpose                                                                                                            |
| :----------------------------------------------- | :------------------ | :------------------------- | :----------------------------------------------------------------------------------------------------------------- |
| `/api/v1/admin/status`                           | GET                 | Org                        | Session / account status probe                                                                                     |
| `/api/v1/admin/users`                            | GET                 | Org                        | List organisation users                                                                                            |
| `/api/v1/admin/users/register`                   | POST                | Org                        | Create a user account                                                                                              |
| `/api/v1/admin/users/{id}/status`                | PATCH               | Org                        | Suspend / activate a user                                                                                          |
| `/api/v1/admin/users/{id}/password`              | POST                | Org                        | Reset a user password                                                                                              |
| `/api/v1/admin/users/{id}`                       | DELETE              | Org                        | Soft-delete a user                                                                                                 |
| `/api/v1/admin/automated-services/register`      | POST                | Org                        | Create a service account                                                                                           |
| `/api/v1/admin/automated-services/{id}/metadata` | PATCH               | Org                        | Update service metadata (jsonb merge) and description                                                              |
| `/api/v1/admin/automated-services/{id}/status`   | PATCH               | Org                        | Operational status: `active` / `degraded` / `maintenance` / `offline` (non-active revokes credentials immediately) |
| `/api/v1/admin/automated-services/rotate-token`  | POST                | Org                        | Rotate a service token                                                                                             |
| `/api/v1/admin/automated-services/{id}`          | DELETE              | Org                        | Soft-delete a service                                                                                              |
| `/api/v1/admin/permissions`                      | GET / POST / DELETE | Org                        | Domain permission grants                                                                                           |
| `/api/v1/admin/roles`                            | GET / POST          | Org                        | RBAC role catalog                                                                                                  |
| `/api/v1/admin/roles/assignments`                | GET                 | Org                        | Who currently holds which role (org-scoped)                                                                        |
| `/api/v1/admin/roles/{id}`                       | PUT / DELETE        | Org                        | Update / delete a role                                                                                             |
| `/api/v1/admin/roles/assign`, `/roles/revoke`    | POST                | Org                        | Role assignment (self-targeting refused)                                                                           |
| `/api/v1/admin/audit-logs`                       | GET / DELETE        | Org / **Platform** (purge) | Read / purge the tamper-evident trail                                                                              |
| `/api/v1/admin/audit-logs/verify`                | GET                 | Org                        | Re-verify the hash chain                                                                                           |
| `/api/v1/admin/quotas`                           | GET                 | Org                        | Quota ledger: `?kind=user\|service\|organisation\|domain&search=&limit=` → `{kind, rows, count}`                   |
| `/api/v1/admin/quotas/user/{id}`                 | PUT                 | Org                        | User caps — all 6 quota columns                                                                                    |
| `/api/v1/admin/quotas/service/{id}`              | PUT                 | Org                        | Service caps — all 6 quota columns                                                                                 |
| `/api/v1/admin/quotas/organisation`              | PUT                 | Org (platform: `?name=`)   | Org caps — storage / entries / API RPM                                                                             |
| `/api/v1/admin/quotas/domain/{id}`               | PUT                 | Org                        | Domain namespace caps — storage / entries                                                                          |
| `/api/v1/admin/organisations`                    | GET                 | Org                        | Registry (org admins see only their own row)                                                                       |
| `/api/v1/admin/organisations`                    | POST                | **Platform**               | Create org + default domain + optional initial admin                                                               |
| `/api/v1/admin/organisations/{id}`               | PATCH               | **Platform**               | Description / default domain / lifecycle status                                                                    |
| `/api/v1/admin/domains/{id}/rename`              | POST                | Org                        | Rename a domain and its authorities                                                                                |
| `/api/v1/admin/domains/{id}/move`                | POST                | Org                        | Re-parent a domain in the namespace tree                                                                           |
| `/api/v1/admin/webhooks`                         | GET / POST          | Org                        | Event webhook registry (`DELETE /{id}` removes)                                                                    |
| `/api/v1/admin/metaprobe/sessions`               | GET                 | Org                        | Live session probe                                                                                                 |
| `/api/v1/admin/metaprobe/sessions/{token}`       | DELETE              | Org                        | Revoke one live session (reason `admin_revoked`)                                                                   |
| `/api/v1/admin/storage/overview`, `/search`      | GET                 | Org                        | Storage overview / search                                                                                          |
| `/api/v1/admin/storage/orphans`                  | GET                 | Org                        | Orphan scan (`/purge` is **Platform**)                                                                             |
| `/api/v1/admin/storage/formats`                  | GET / POST / DELETE | Org                        | Storage format catalog                                                                                             |
| `/api/v1/admin/system/config`                    | GET / PUT           | **Platform**               | System-wide configuration                                                                                          |
| `/api/v1/admin/analytics/overview`, `/breakdown` | GET                 | Org                        | Storage analytics                                                                                                  |

### Who can change what

| Target                         | Org admin                                | Platform admin                   | Notes                                                      |
| :----------------------------- | :--------------------------------------- | :------------------------------- | :--------------------------------------------------------- |
| Users of own org               | ✅ register / status / password / delete | ✅ any org                       | Self-status changes and self-deletes are refused           |     | Services of own org | ✅ register / metadata / status / token / delete | ✅ any org | Non-`active` status revokes the service credentials immediately |
| Own organisation row           | ❌ (read-only)                           | ✅ create / PATCH status         | Suspending/archiving your own org is refused               |
| Other organisations            | ❌                                       | ✅ via `?name=` on quota updates |                                                            |
| Domains of own org             | ✅ create / rename / move / quota        | ✅                               | Rename cascades to sub-tree authority labels               |
| Quotas (user / service)        | ✅                                       | ✅                               | 6 columns; absent key = untouched, `null` = lift cap       |
| Quotas (org / domain)          | ✅                                       | ✅                               | Org: first 3 columns, domain: first 2 — extra keys ignored |
| Permissions / roles            | ✅ (own org only)                        | ✅                               | Self-grant, self-assign and above-level grants refused     |
| Audit logs                     | read / verify                            | purge                            | Hash-chained; tamper-evident                               |
| Storage orphans, system config | read                                     | purge / write                    |                                                            |

### Behavioural contracts

- **Partial quota semantics** (F6): request keys absent → column untouched;
  explicit `null` → cap lifted; integer → validated non-negative and
  bounded. Sending only `storage_limit` no longer silently clears the other
  caps.
- **Organisation naming:** names render into `<org>-<domain>` authorities, so
  the server (and dashboard) reject empty names, names > 64 chars, and any
  of whitespace, `.` `@` `/` `-`.
- **Lifecycle semantics** (migration `008`): suspending / soft-deleting a
  user or service, or suspending an organisation, terminates live sessions
  immediately; `invited` users cannot sign in; suspending/archiving your own
  organisation is refused.
- **Audit trail:** every admin mutation and every generated CRUD write leaves
  one `core.audit_logs` row (`crud.<table>.<op>` for generated views) with
  the acting session's identity; arbitrary client JSON keys are never echoed
  into the details payload.
- **Domain caps** are admin-route-only — never client-writable through
  generated CRUD (see `scripts/generators/README.md`).
