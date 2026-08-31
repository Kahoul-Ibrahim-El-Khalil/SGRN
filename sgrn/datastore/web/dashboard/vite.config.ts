import react from "@vitejs/plugin-react";
import path from "path";
import { defineConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";

// ===========================
// ⚙️ Vite Configuration
// ===========================
export default defineConfig({
    plugins: [
        react({
            babel: {
                plugins: [["babel-plugin-react-compiler"]],
            },
        }),
        viteSingleFile(),
    ],
    resolve: {
        alias: {
            "@": path.resolve(__dirname, "./src"),
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
});
