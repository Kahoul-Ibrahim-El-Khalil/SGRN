import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig, type Plugin } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

/**
 * Worker path fix — viteSingleFile does not inline workers, so they're
 * emitted as separate files.  Vite generates:
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
        if (chunk.type === "asset" && typeof chunk.source === "string" && chunk.fileName.endsWith(".html")) {
          chunk.source = chunk.source.replace(/\.\.\/(worker-[^"']+\.js)/g, "./$1");
        }
      }
    },
  };
}

export default defineConfig({
  plugins: [
    svelte(),
    viteSingleFile(),
    fixWorkerUrlPlugin(),
  ],
  base: "./",
  resolve: {
    alias: {
      "@sgrn/gateway": path.resolve(
        __dirname,
        "../../bindings/typescript/gateway/src/index.ts"
      ),
      "@docs": path.resolve(
        __dirname,
        "../../../documentation/gateway"
      ),
    },
  },
  build: {
    outDir: "dist",
    assetsDir: "assets",
    assetsInlineLimit: 4096,
    minify: true,
    rollupOptions: {
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
      "/ws": { target: "ws://localhost:8001", ws: true },
    },
  },
});
