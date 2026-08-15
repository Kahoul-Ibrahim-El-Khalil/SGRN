# Admin API

All routes are under `/api/v1/admin/...`.

Auth:

- Requires a user session (`UserAuthFilter`)
- Requires admin privileges (`AdminFilter`)

## Server Status

`GET /api/v1/admin/status`

Returns backend status and related runtime info.

## List Users

`GET /api/v1/admin/users`

Returns user records (admin-only).

## Register User

`POST /api/v1/admin/users/register`

Creates a new user record.

## Register Agent

`POST /api/v1/admin/agents/register`

Creates a new agent.

Organization binding:

- Agents are created inside the authenticated admin user's organisation automatically.
- The back end calls `core.create_agent(name, kind, metadata, organisation_id)` so the returned agent belongs to the same organisation as the admin.
- The response returns the raw `token_secret` exactly once; copy it immediately.

Body:

```json
{
  "name": "plc-gateway-1",
  "kind": "plc",
  "metadata": { "site": "line-a" }
}
```

## List Agents

`GET /api/v1/admin/agents`

Returns every agent that belongs to the authenticated admin's organisation.

Response shape (per row):
```json
{
  "id": 1,
  "name": "line-a-polling",
  "kind": "plc",
  "token": "xxxxxxxxxxxx",
  "metadata": { /* JSON metadata blob */ },
  "is_active": true,
  "created_at": "2026-03-01T08:42:00Z"
}
```

The JSON metadata allows the admin UI or tooling to display contextual tags (site, line, purpose, etc.).

## Rotate Agent Tokens

`POST /api/v1/admin/agents/rotate-token`

Body:
```json
{ "agent_id": 42 }
```

Rotates both the token and token_secret for the requested agent, using the `core.rotate_agent_credentials(agent_id, true)` helper. The response mirrors `core.create_agent(...)`, including the new `token_secret`. This is the only time the new secret is exposed, so store it securely.

Access is restricted to agents that belong to the same organisation as the caller; cross-org requests return `403 Forbidden`.

## Endpoint List

`GET /api/v1/endpoints`

Returns a cached list of known backend endpoints.

## MetaProbe Sessions

`GET /api/v1/admin/metaprobe/sessions`

Admin-only session listing for MetaProbe tooling.
