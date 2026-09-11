# sgrn/web — embedded web UIs

One directory per UI. Both are built by CMake (`bun run build`) and baked
into their server binaries as compressed assets — there is no separate web
deploy step. `node_modules/`, `dist/` and `build/` are git-ignored
regenerables; run `bun install` in the UI directory first.

- **`gateway/`** — Svelte SPA served by the gateway's HTTP adapter.
  Depends on `@sgrn/gateway` (`file:../../typescript/gateway`, aliased in
  `vite.config.ts`). Output `dist/` becomes `web_assets.hpp`.
- **`datastore/`** — React dashboard served by the datastore backend.
  Depends on `@sgrn/types` (`file:../../typescript/types`). Has its own
  eslint/prettier/tsconfig base, `scripts/` (unreferenced dev scratch
  against the datastore API), and `gateway_config.json`. Output `build/`
  becomes `web_assets.hpp` via `sgrn/lib/datastore`.

Relevant CMake: `WEB_*` vars in `sgrn/lib/gateway/CMakeLists.txt`,
`DASHBOARD_*` vars in `sgrn/lib/datastore/src/CMakeLists.txt`.
