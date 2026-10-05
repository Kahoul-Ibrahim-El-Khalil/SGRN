import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig, type Plugin } from "vite";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

/**
 * Worker path fix — workers are emitted as separate files. Vite generates:
 *   new URL("../worker-<hash>.js", import.meta.url)
 * The "../" is relative to the assumed assetsDir (assets/), but with
 * <base href="/gateway/"> injected at runtime the "../" escapes the
 * gateway prefix, resolving to https://localhost/worker-<hash>.js
 * instead of https://localhost/gateway/worker-<hash>.js.
 *
 * This plugin rewrites "../worker-" → "./worker-" so the URL stays
 * under the base path.
 */
function fixWorkerUrlPlugin(): Plugin {
  return {
    name: "fix-worker-url",
    enforce: "post",
    generateBundle(_options, bundle) {
      for (const [, chunk] of Object.entries(bundle)) {
        if (
          chunk.type === "asset" &&
          typeof chunk.source === "string" &&
          chunk.fileName.endsWith(".html")
        ) {
          chunk.source = chunk.source.replace(
            /\.\.\/(worker-[^"']+\.js)/g,
            "./$1",
          );
        }
      }
    },
  };
}

export default defineConfig({
  // NOTE: no viteSingleFile — the two dashboard variants (index, replay)
  // share code via emitted chunks (true inheritance: common components load
  // once, docs chunk only referenced by index.html). All dist files are
  // embedded + served by HttpAdapter::registerWebAssets.
  plugins: [svelte(), fixWorkerUrlPlugin()],
  base: "./",
  resolve: {
    alias: {
      "@sgrn/gateway": path.resolve(
        __dirname,
        "../../typescript/gateway/src/index.ts",
      ),
      "@docs": path.resolve(__dirname, "../../../documentation/gateway"),
    },
  },
  build: {
    outDir: "dist",
    assetsDir: "assets",
    assetsInlineLimit: 4096,
    minify: true,
    rollupOptions: {
      // Two dashboard variants sharing components (inheritance, not copies):
      // index.html = full gateway dashboard (incl. docs), replay.html =
      // replay-only dashboard (process image + pacing, no docs bundle).
      input: {
        index: path.resolve(__dirname, "index.html"),
        replay: path.resolve(__dirname, "replay.html"),
      },
      output: {
        entryFileNames: "assets/[name]-[hash].js",
        chunkFileNames: "assets/[name]-[hash].js",
        assetFileNames: "assets/[name]-[hash][extname]",
      },
    },
  },
  worker: {
    format: "es",
  },
  // Proxy API calls to the running gateway during `bun run dev`
  server: {
    port: 5173,
    proxy: {
      "/api": "http://localhost:8000",
      "/data": "http://localhost:8000",
      "/registry": "http://localhost:8000",
      "/connections": "http://localhost:8000",
      "/db": "http://localhost:8000",
      "/endpoints": "http://localhost:8000",
      "/replay": "http://localhost:8000",
      "/ws": { target: "ws://localhost:8000", ws: true },
    },
  },
});
