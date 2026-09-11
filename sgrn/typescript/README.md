# sgrn/typescript — shared TypeScript packages (bun workspace)

- **`gateway/`** (`@sgrn/gateway`) — typed client for the gateway REST +
  telemetry API. Consumed by `sgrn/web/gateway`.
- **`datastore/`** (`@sgrn/datastore`) — datastore API client. Imports
  `@sgrn/types` via a relative `file:` dependency.
- **`types/`** (`@sgrn/types`) — shared domain types (imported by the
  dashboard, the datastore client, and the error-scope lint).
- **`scripts/`** — unreferenced dev scratch scripts against the datastore
  API (run with `bun scripts/<name>.ts` from here so `@sgrn/datastore`
  resolves via the root `devDependencies`).

Bun resolves nested `file:` dependencies from the installing project root,
so keep this rule: packages consumed *outside* this workspace must only
depend inward via paths that resolve from their own directory (see the
`@sgrn/types` wiring). Run `bun install` here after moves or dep edits.

`scripts/lints/check_error_scopes.sh` cross-checks `@sgrn/types` error
scopes against the C++ backend — keep them in sync.
