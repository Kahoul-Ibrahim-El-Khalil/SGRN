import { vitePreprocess } from "@sveltejs/vite-plugin-svelte";
import { mdsvex } from "mdsvex";

/** @type {import('@sveltejs/vite-plugin-svelte').SvelteConfig} */
const config = {
  extensions: [".svelte", ".md"],
  preprocess: [
    vitePreprocess(),
    mdsvex({
      extensions: [".md"],
      layout: {
        _: "./src/layouts/DocsLayout.svelte",
      },
    }),
  ],
};

export default config;
