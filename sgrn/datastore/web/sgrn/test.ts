process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0";

import { AutomatedServiceClient } from "./AutomatedServiceClient.ts";
import { DirectoryTree } from "./utils/DirTree.ts";
import type { Credentials } from "@sgrn/types";
import { isError, isSuccess } from "@sgrn/types";

const SERVER_URL: string = "https://localhost:8443";

const credentials: Credentials = {
    token: "pu4lZAkMuMTqxsWSc8juzQ",
    secret: "by1bTYYBcx6u3aRD5zuiakJ4RuH4MtoPWXpr7E9PbaQ",
};

// ── Client setup ──────────────────────────────────────────────────────────────

const client = new AutomatedServiceClient(credentials, SERVER_URL);
const sign_in_res = await client.signIn();
if (isError(sign_in_res)) {
    console.error(`✗ Sign-in failed: ${sign_in_res.error} [Scope: ${sign_in_res.scope}]`);
    process.exit(1);
}
console.log(`✓ Signed in  (session: ${client.session?.session_id ?? "n/a"})`);

// ── Config ────────────────────────────────────────────────────────────────────

/** Root directory to mirror onto the server. Change this to any path you want. */
const TARGET_DIR = process.argv[2] ?? process.cwd();

// ── Build tree ────────────────────────────────────────────────────────────────

const tree_res = DirectoryTree.build(TARGET_DIR);
if (isError(tree_res)) {
    console.error(`✗ Tree build failed: ${tree_res.error}`);
    process.exit(1);
}
const tree = tree_res.data;
console.log(`\n✓ Tree built  (root: ${tree.head.absolute_path})\n`);

// ── Upload every file, preserving absolute paths as virtual paths ─────────────

const results: Array<{ local: string; virtual: string; ok: boolean }> = [];

// Collect all file nodes first so we can show a summary even on partial failure
const file_nodes: Array<{ local: string; virtual: string }> = [];

tree.traverse((node) => {
    if (node.isFile()) {
        file_nodes.push({
            local: node.absolute_path, // read from here
            virtual: node.absolute_path, // mirror the real absolute path on the server
        });
    }
});

console.log(`Files to upload: ${file_nodes.length}\n`);

for (const { local, virtual } of file_nodes) {
    const result = await client.uploadFileFromDisk(local, virtual);
    if (isSuccess(result)) {
        console.log(`  ✓  ${virtual}`);
        if (result.data.deduplicated) console.log(`     └─ deduplicated (already stored)`);
        results.push({ local, virtual, ok: true });
    } else {
        console.error(`  ✗  ${virtual}`);
        console.error(`     └─ ${result.error} [Scope: ${result.scope}]`);
        results.push({ local, virtual, ok: false });
    }
}

// ── Summary ───────────────────────────────────────────────────────────────────

const succeeded = results.filter((r) => r.ok).length;
const failed = results.length - succeeded;
console.log(`\n── Upload complete ──────────────────────────────────────`);
console.log(`   ✓ ${succeeded} succeeded`);
if (failed > 0) console.log(`   ✗ ${failed} failed`);

// ── Cleanup ───────────────────────────────────────────────────────────────────

await client.signOut();
console.log(`\n✓ Signed out`);
