# sgrn/web — embedded web UIs

One directory per UI. Both are built by **CMake + Bun + Vite** and baked into
their server binaries as Zstd-compressed assets. There is **no separate web
deploy step** in production: the SPA ships inside `sgrn_datastore` or
`sgrn_gateway`.

`node_modules/`, `dist/`, and `build/` are git-ignored regenerables.

## Directories

| Directory | Stack | Served by | Output dir | Embedded as |
|---|---|---|---|---|
| **`datastore/`** | React + Vite | Drogon (`sgrn_datastore`) | `build/` | `dashboard_generated/web_assets.hpp` |
| **`gateway/`** | Svelte + Vite | Crow (`sgrn_gateway`) | `dist/` | `web_generated/web_assets.hpp` |

TypeScript bindings consumed at build time:

- Dashboard: `@sgrn/types` (`sgrn/typescript/types/`)
- Gateway: `@sgrn/gateway` (`sgrn/typescript/gateway/`)

## Why Bun is required

CMake does **not** call `npm` or `pnpm`. The build graph runs:

```text
bun install          # dashboard only, when lockfiles change
bun run build        # Vite production bundle for both UIs
```

You must have [Bun](https://bun.sh) on `PATH` when building
`sgrn_datastore`, `sgrn_gateway`, or the asset targets
(`sgrn_dashboard_assets`, etc.). After Vite succeeds, Python
`scripts/generate_embedded_assets.py` compresses each file into C++ headers
(needs `pip install zstandard` or the SGRN conda env).

Normal workflow — edit UI sources, then:

```bash
cmake --build <build-dir> --target sgrn_datastore   # dashboard
cmake --build <build-dir> --target sgrn_gateway       # gateway SPA
```

Never commit `web_assets.hpp`; it lives under the CMake **binary dir** only.

## How embedding works at runtime

1. Vite emits static files (`index.html`, hashed JS/CSS under `assets/`).
2. `generate_embedded_assets.py` reads each file, Zstd-compresses it, and
   writes per-file headers plus a master `web_assets.hpp` with an
   `AssetRegistry` (`sgrn::EmbeddedAsset` array in `.rodata`).
3. The server registers **one GET route per virtual path** (e.g. `/index.html`,
   `/assets/index-abc123.js`).
4. Requests with `Accept-Encoding: zstd` receive compressed bytes directly;
   others get one-time decompress + cached `HttpResponse`.

**Datastore:** built with `VITE_BASE_PATH=/datastore`; nginx typically mounts
the UI at `https://host/datastore/…`. `registerDashboardAssets()` also aliases
`GET /` to `index.html` when present.

**Gateway:** built with base `/`; runtime patches `index.html` for subpath
deploy using nginx `X-Forwarded-Prefix` (see `gateway/adapters/http/assets.cpp`).

Full pipeline tables: [`generators/README.md` — Embedded VFS](../lib/datastore/scripts/generators/README.md#embedded-vfs-asset-generators-scripts).

## Frontend development (datastore dashboard)

Feature work checklist:

1. API paths → `sgrn/web/datastore/src/backend/endpoints.ts`
2. Fetch wrapper → `src/backend/api/fetcher.ts` (`authenticatedFetch`)
3. Pages/components → `src/pages/…`
4. Rebuild binary to refresh embedded assets (see above)

Generated CRUD APIs (`/api/v1/domains`, `/api/v1/users`, …) use PostgREST-style
query filters — documented in [`documentation/datastore/crud_views.md`](../../documentation/datastore/crud_views.md).

Broader backend + SQL + scope boundaries:
[`documentation/datastore/development.md`](../../documentation/datastore/development.md).

## CMake entry points

- Datastore: `DASHBOARD_*` in `sgrn/lib/datastore/src/CMakeLists.txt`
- Gateway: `WEB_*` in `sgrn/lib/gateway/CMakeLists.txt`
