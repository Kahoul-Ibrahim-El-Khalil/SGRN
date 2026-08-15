# GPAO API (Production Management)

All routes are under `/api/v1/gpao/...`.

Auth:

- Requires a user session (`UserAuthFilter`) for reads and inserts.
- Delete and some updates require admin (`AdminFilter`).

This API follows a consistent pattern across entities:

- `GET /api/v1/gpao/<entity>`: list
- `POST /api/v1/gpao/<entity>`: insert
- `DELETE /api/v1/gpao/<entity>`: delete (admin-only)
- `PATCH /api/v1/gpao/<entity>`: update (admin-only, where applicable)
- `GET /api/v1/gpao/<entity>/schema`: JSON schema for UI forms
- `GET /api/v1/gpao/<entity>/form`: insertion form description
- `GET /api/v1/gpao/<entity>/by-id?id=<id>`: fetch a single record

## Helper

`GET /api/v1/gpao/pairs?table=<table_name>`

Returns `{id, name}` pairs for UI dropdowns.

## Entities

- `machine-manufacturers`
- `machine-models`
- `machines`
- `products`
- `orders`
- `operations`

