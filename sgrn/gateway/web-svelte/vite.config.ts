import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vite";
import path from 'node:path'

import { fileURLToPath } from 'node:url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

export default defineConfig({
  plugins: [svelte()],
  base: "./",
     resolve: {
        alias: {
            "@docs": path.resolve(
                __dirname,
                "../../../documentation/gateway"
            ),
        },
    },  build: {
    outDir: "dist",
    assetsDir: "assets",
    // Inline small assets directly in the HTML to reduce round-trips
    assetsInlineLimit: 4096,
    // Compact output for binary embedding
    minify: true,
    rollupOptions: {
      output: {
        // Deterministic filenames — Python asset baker reads these
        entryFileNames: "assets/[name]-[hash].js",
        chunkFileNames: "assets/[name]-[hash].js",
        assetFileNames: "assets/[name]-[hash][extname]",
        // Keep the worker as a separate chunk
        manualChunks(id) {
          if (id.includes("worker")) return "worker";
        },
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
