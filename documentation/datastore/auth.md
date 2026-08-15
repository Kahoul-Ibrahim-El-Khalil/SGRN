# Auth API

All routes are under `/api/v1/auth/...`.

## User Sign-In

`POST /api/v1/auth/user/signin`

Body:
```json
{
  "email": "user@example.com",
  "password": "..."
}
```

Response:

- `200 OK` with JSON containing a Session Token (also returned in `Authorization: Bearer ...` header).
- `400 Bad Request` on invalid JSON or missing fields.
- `401 Unauthorized` on bad credentials.

## User Sign-Out

`POST /api/v1/auth/user/signout`

Auth: `Authorization: Bearer <token>` (user session token)

Response:

- `302 Found` (redirect) or JSON depending on deployment conventions.

## Update User Password

`POST /api/v1/auth/user/password`

Auth: `Authorization: Bearer <token>` (user session token)

Body:
```json
{
  "old_password": "...",
  "new_password": "..."
}
```

## Agent Sign-In

`POST /api/v1/auth/agent/signin`

Body:
```json
{
  "token": "<agent public token>",
  "secret": "<agent token_secret>"
}
```

Response:

- `200 OK` with JSON: `{ "token": "<token>", "agent": { ... }, "session_id": <int> }`
- The `token` field in the response is an opaque Session Token (UUID). Use it as `Authorization: Bearer <token>`.

Notes:

- The backend stores only a hash of `token_secret` (bcrypt via `crypt()`); you cannot recover a lost secret. You must rotate credentials.
- Agent sign-in is implemented so that the agent's `session_id` is stable across re-auth (important for storage VFS ownership).
- Agents belong to an organisation; the sign-in claims include `organisation.id` and `organisation.name`.

## Agent Session Info

`GET /api/v1/auth/agent/session`

Auth: `Authorization: Bearer <token>` (agent session token)

Response:

- `200 OK` with JSON describing `agent_id`, `agent_kind`, `session_id`.

## Agent Sign-Out

`POST /api/v1/auth/agent/signout`

Auth: `Authorization: Bearer <token>` (agent session token)

Behavior:

- Terminates the `core.sessions` row for the given Session Token.
- Evicts the token from Redis.

## Credential Rotation (Database Function)

Credential rotation is a database function, not an HTTP endpoint.

See:

- `docs/db/functions.md` for `core.rotate_agent_credentials(...)`.
