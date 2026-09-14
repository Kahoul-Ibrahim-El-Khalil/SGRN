# Storage API (Virtual Filesystem)

Storage is a virtual filesystem on top of:

- Postgres metadata tables: `storage.files`, `storage.directories`, `storage.objects`
- S3-compatible object store content (Garage), referenced by unique object hashes.

All routes are under `/api/v1/storage/...`.

## Upload/Download (User)

These endpoints use `UserAuthFilter`.

Upload:

- `POST /api/v1/storage/files?path=/folder/file.ext`
- Body: `multipart/form-data`
- One file part is required. The `path` parameter determines the virtual location of the file.

Download:

- `GET /api/v1/storage/files?path=/folder/file.ext`

Optional query params:

- `scope`: `personal` (default), `automated_services`, `users` (admin-only scopes)

## Upload/Download (Agent)

These endpoints use `AgentAuthFilter`.

Upload:

- `POST /api/v1/storage/agent/files?path=/folder/file.ext`
- Body: `multipart/form-data`

Download:

- `GET /api/v1/storage/agent/files?path=/folder/file.ext`

Important:

- Do not send `Content-Encoding: zstd` for multipart uploads.
- Downloads may return compressed objects (`Content-Type: application/zstd` and a `.zst` filename) if the backend chose to store the object compressed.

## File Metadata

`GET /api/v1/storage/files/metadata`

Auth: User Session Token (Bearer)

This is intended as a metadata view for UI dashboards (internally backed by Postgres views).

## Drive Operations

All use `UserAuthFilter`:

- `GET /api/v1/storage/drive/list`
  - Query params: `scope` and `path`
  - Response includes `path`, `trail`, `folders`, and `files`.
  - Admin users can browse virtual roots for `scope=automated_services` and `scope=users` to see the namespace roots.
- `POST /api/v1/storage/drive/mkdir?path=/path/to/new/folder`
- `PATCH /api/v1/storage/drive/move?id=123`
  - Body may contain `parent_id` to move into a folder.
  - Body may contain `new_name` to rename while moving, or `name` as a legacy alias.
- `DELETE /api/v1/storage/drive/delete?id=123`
- `GET /api/v1/storage/drive/zip?path=/folder`

## Drive Response Shape

The drive list endpoint returns:

- `path`: the current virtual path within the selected scope
- `trail`: breadcrumb nodes with `id`, `name`, `path`, and optional `display_name`
- `folders`: folder entries. For `scope=automated_services`, entries include `name` as the stable service token and `display_name` as the human label.
- `files`: file entries with their virtual path and metadata

For `scope=automated_services`, the virtual root exposes service tokens, not raw database ids, so the UI can navigate by token while still showing the service name.

## Storage Constraints

`GET /api/v1/storage/constraints`

Public endpoint that returns:

- maximum file size
- compression thresholds
- allowed extensions list

## Capacity & Scaling

Two different questions: how much in total, and how large per file.

**Total volume** is disk-bound. Garage holds whatever its data dirs fit;
grow it by raising the layout capacity and adding nodes/disks. Postgres
metadata scales to millions of object rows, and content-hash dedup stores
repeated bytes once.

**Per-file ceilings** (all must move together; raising one alone changes nothing):

- `custom_config.s3.max_file_size_mb` + drogon `client_max_body_size` +
  nginx `client_max_body_size`: hard reject above the smallest.
- S3 part budget (~10k parts/object) × `chunk_part_size_mb`: e.g. ~48 GB
  per object at 5 MB parts, ~117 GB at 12 MB.
- Downloads buffer the whole object in RAM (~2× file size transiently:
  S3 body + HTTP response). Files approaching host RAM need ranged/streaming
  downloads; `getObjectContent` cannot serve them.
- Streaming uploads spill temp files (original + compressed copy); staging
  must live on a disk-backed directory, not a small tmpfs, before large
  files are attempted.
- Governance tooling pages: census scans max 100×1000 keys, orphan listing
  caps at 5000 — reports truncate (flagged) past ~100k objects.

**Durability**: a single node with `replication_factor=1` has no redundancy.
Anything irreplaceable wants replicas across nodes/zones plus offsite backup
before volume grows.

## Configuring Garage (Object Store)

Garage is a plain S3-compatible backend: the datastore only needs an
endpoint, a region, and a key with read/write on its bucket.

**1. Install.** Any `garage` v2.x binary on `PATH`
(`/usr/local/bin`, `~/bin`, or the deployment env's `bin/`).

**2. `garage.toml`.** Minimal single-node file:

```toml
metadata_dir = "<data-dir>/garage/meta"
data_dir     = "<data-dir>/garage/data"
db_engine = "lmdb"
replication_factor = 1
consistency_mode = "consistent"

rpc_bind_addr = "[::]:3901"
rpc_public_addr = "127.0.0.1:3901"
rpc_secret = "<openssl rand -hex 32>"
bootstrap_peers = []

[s3_api]
s3_region = "us-east-1"        # MUST equal the datastore's s3 region:
                               # Garage rejects other regions outright
api_bind_addr = "[::]:3900"
root_domain = ".s3.garage"

[s3_web]
bind_addr = "[::]:3902"
root_domain = ".web.garage"
index = "index.html"

[admin]
api_bind_addr = "[::]:3903"    # unauthenticated /health backs readiness gates
admin_token = "<openssl rand -hex 32>"
```

The repo ships this as `configs/garage/garage.toml`, templated from `.env`
(`GARAGE_*`); `garage.toml` is a pure function of `.env`, so regenerating it
is always safe (unlike `sgrn.json`, which carries dashboard-tuned values).

**3. Start + single-node layout:**

```bash
garage -c garage.toml server
garage -c garage.toml status            # note the node id
garage -c garage.toml layout assign -z dc1 -c <capacity, e.g. 50G> <node-id>
garage -c garage.toml layout apply --version 1
```

Multi-node: same file on every host (identical `replication_factor`,
per-host `metadata_dir`/`data_dir`), `replication_factor` 2–3, peers in
`bootstrap_peers`, assign each node before `layout apply`.

**4. Key + bucket** (names must match `sgrn.json`'s `default_bucket`):

```bash
garage -c garage.toml key import <access-key> <secret-key> -n sgrn-datastore --yes
# …or: garage -c garage.toml key create sgrn-datastore   (generates credentials)
garage -c garage.toml bucket create sgrn-uploads
garage -c garage.toml bucket allow --read --write sgrn-uploads --key sgrn-datastore
```

**5. Point the datastore at it** (`sgrn.json`, `plugins` → `S3Client`):

```json
{ "region": "us-east-1", "endpoint": "http://127.0.0.1:3900",
  "access_key": "<key>", "secret_key": "<secret>" }
```

Restart required (plugin config is boot-only, unlike hot `custom_config.s3.*`).
systemd should order after the store with a readiness gate on
`http://127.0.0.1:3903/health` — `After=` alone never waits for traffic.

**6. Migrating from another S3 store.** Keys are content hashes, so a plain
copy preserves every DB reference: mirror bucket-to-bucket (`mc mirror`,
`rclone sync`, or `aws s3 sync` per side), verify sizes/checksums, then flip
`provider` on existing `storage.objects` rows — `upsert_object` treats a
provider mismatch as a conflict, so rows must move with the bytes.

**7. Verify.** `curl -sf :3903/health`; `HeadObject` on a known multipart
object shows an ETag ending in `-N` (N = assembled parts); the upload receipt
and Details panel show the same without touching the store.

## Large Files: Client-Side Chunking

There are two transfer legs, with different chunking stories:

1. **Server → store**: S3 multipart already exists (`chunking_threshold_mb` /
   `chunk_part_size_mb`), traced per object (`upload_mode`, `part_count`,
   `part_size_bytes` on `storage.objects`, exposed in upload receipts, file
   metadata, drive listing, and the Details panel).
2. **Client → server**: one HTTP POST per file today. This leg breaks first
   for large files — no resume on failure, full-body buffering against
   `client_max_body_size`, proxy timeouts. Server-side multipart cannot fix it.

So yes: large files need **client-side (browser) chunked, resumable upload**:
`Blob.slice` into N-MB chunks under a session id, server appends chunks to a
staging file, then the existing pipeline runs unchanged
(hash → compress → S3 multipart → DB). All current guarantees (dedup,
compression, chunking trace) survive because the pipeline entry point doesn't
move — only the transport to it changes. Chunk sessions should record their
own trace (session id, chunk count, checksums) the same way objects do.

A later, bigger step is presigned direct-to-Garage upload (bytes bypass the
server). That changes the trust model — the client writes the store, so
hashing/compression/dedup either move client-side or the server re-verifies
after — and is only worth it once (1) above is proven insufficient.
