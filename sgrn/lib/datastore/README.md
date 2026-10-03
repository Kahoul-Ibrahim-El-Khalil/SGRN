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
- **Timeline Reconstruction:** Replays Deltas over the nearest Anchor to satisfy temporal queries (*e.g.*, "What was the exact memory map of DB2 at timestamp T?") with minimal storage overhead.

### 3. Forwarding & Storage Orchestration
- **TimescaleDB / PostgreSQL:** Dynamic SQL persistence for analytical query models.
- **Garage / S3 Object Storage:** Cold storage and immutable Parquet/JSONL block archiving.
- **Resilience & Queueing:** Automatic local buffer fallback during backend outages with smooth recovery queue draining.

---

## 🌐 HTTP REST API Reference

All endpoints are hosted under `/api/v1/`.

| Endpoint | Method | Scope | Description |
| :--- | :--- | :--- | :--- |
| `/api/v1/auth/signin` | `POST` | Public | Authenticate user credentials and obtain session token |
| `/api/v1/auth/signout` | `POST` | User | Terminate current user session |
| `/api/v1/storage/list` | `GET` | User | List directory contents (virtual Drive space) |
| `/api/v1/storage/upload` | `POST` | User | Direct single-file upload (files < 5MB) |
| `/api/v1/storage/upload/batch` | `POST` | User | Multi-file and directory structure batch upload |
| **Resumable Uploads** | | | |
| `/api/v1/storage/upload/init` | `POST` | User | Initiate a resumable chunked upload session |
| `/api/v1/storage/upload/chunk` | `PUT` | User | Upload a binary chunk (`upload_id`, `chunk_index`) |
| `/api/v1/storage/upload/status` | `GET` | User | Query upload progress & uploaded chunk indices |
| `/api/v1/storage/upload/complete` | `POST` | User | Finalize and assemble uploaded chunks into storage object |
| `/api/v1/storage/upload/abort` | `DELETE` | User | Cancel session and clean up temporary chunks |
| **Resumable Downloads** | | | |
| `/api/v1/storage/download` | `GET` | User | Standard & HTTP Range (resumable) object download |
| **Admin & Telemetry** | | | |
| `/api/v1/admin/metaprobe/sessions` | `GET` | Admin | Query active user and service authentication sessions |
| `/api/v1/admin/webhooks` | `GET` | Admin | List registered event webhooks |
| `/api/v1/admin/webhooks` | `POST` | Admin | Register new webhook for auth events (`user.signin`, `user.signout`) |
| `/api/v1/admin/webhooks/{id}` | `DELETE` | Admin | Remove registered webhook endpoint |

---

## 💻 Datastore Shell CLI & AngelScript REPL

The `sgrn_datastore_shell` CLI provides full administrative and storage management capabilities with built-in AngelScript automation.

### Available Commands

| Command | Arguments | Description |
| :--- | :--- | :--- |
| `rput` | `<LOCAL> [REMOTE] [--chunk-size B]` | Resumable chunked upload with interactive progress bar |
| `upload-status` | `<UPLOAD-ID>` | Inspect status and uploaded chunk map of a resumable upload |
| `upload-abort` | `<UPLOAD-ID>` | Abort an ongoing upload session |
| `sessions` | — | Display active user sessions table (admin) |
| `webhooks` | — | List all registered event webhooks (admin) |
| `webhook-add` | `<URL> [--secret S]` | Register a new webhook endpoint (admin) |
| `webhook-del` | `<ID>` | Remove a webhook endpoint by ID (admin) |
| `ls`, `cd`, `pwd` | `[PATH]` | Navigate virtual storage hierarchy |
| `get`, `put` | `<FILE>` | Download / upload single files |
| `mkdir`, `rm` | `<PATH>` | Manage directories and objects |
| `whoami`, `logout` | — | Inspect session identity or sign out |

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
- **Admin Metaprobe:** Live user session probe, audit logs, and webhook subscriptions.
- **Analytics & Health:** Ingest rate monitoring, storage orphan detection, and quota enforcement.
