import react from "@vitejs/plugin-react";
import path from "path";
import { defineConfig, type UserConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";

// ===========================
// ⚙️ Vite Configuration
// ===========================
export default defineConfig(({ command }): UserConfig => {
    // `bun run dev` serves the dashboard at the server root — NOT behind
    // nginx's /datastore prefix — so the router must mount with basename "/".
    // src/lib/appPath.ts falls back to "/datastore" (the production contract),
    // which made "/" an out-of-basename path: nothing matched → white page.
    // This dev-only define pins VITE_BASE_PATH to "/" (honoring an explicit
    // VITE_BASE_PATH env if one was exported). Production builds are
    // unaffected: `define` is empty outside serve, so the CMake-injected
    // VITE_BASE_PATH still rules.
    const serveOnlyDefine: UserConfig["define"] | undefined =
        command === "serve" ? { "import.meta.env.VITE_BASE_PATH": JSON.stringify(process.env.VITE_BASE_PATH ?? "/") } : undefined;

    return {
        plugins: [
            react({
                babel: {
                    plugins: [["babel-plugin-react-compiler"]],
                },
            }),
            viteSingleFile(),
        ],
        define: serveOnlyDefine,
        resolve: {
            alias: {
                "@": path.resolve(__dirname, "./src"),
                // Serve the TS types package straight from source so edits to
                // sgrn/typescript/types/src hot-reload during `bun run dev`
                // (instead of the stale copy under node_modules/@sgrn/types).
                "@sgrn/types": path.resolve(__dirname, "../../typescript/types/src/index.ts"),
            },
        },
        build: {
            outDir: "./build",
            assetsDir: "assets",
            emptyOutDir: true,
            rollupOptions: {
                output: {
                    assetFileNames: "assets/[name].[hash][extname]",
                    chunkFileNames: "assets/[name].[hash].js",
                    entryFileNames: "assets/[name].[hash].js",
                },
            },
        },
        base: "./",
        publicDir: "public",
        // ── Development proxy ──────────────────────────────────────────────
        // `bun run dev` from this directory launches sgrn_datastore on port
        // 5001 (dev port — separate from the production 5000 instance) via
        // dev.ts, then starts this Vite server. All /api calls are forwarded
        // to it. Auth is Bearer-token-based so no cookie/SameSite issues.
        server: {
            port: 5173,
            proxy: {
                // All datastore API routes live under /api/v1/.
                // dev.ts exports SGRN_DEV_PORT (auto-bumped when the default
                // is occupied) — the proxy target follows it.
                "/api": {
                    target: `http://localhost:${process.env.SGRN_DEV_PORT ?? "5001"}`,
                    changeOrigin: true,
                },
            },
        },
    };
});
