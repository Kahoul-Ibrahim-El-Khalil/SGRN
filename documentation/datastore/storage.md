# Storage API (Virtual Filesystem)

Storage is a virtual filesystem on top of:

- Postgres metadata tables: `storage.files`, `storage.directories`, `storage.objects`
- S3-compatible object store content (Minio), referenced by unique object hashes.

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
