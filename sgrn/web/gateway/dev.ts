#!/usr/bin/env bun
/**
 * sgrn/web/gateway/dev.ts
 *
 * `bun run dev` entry point for the gateway dashboard.
 *
 * Starts the gateway using the dev schema and config owned by this
 * directory (sgrn/web/gateway/dev/). No real PLC needed.
 *
 * NOTE: do NOT pass --headless here. Headless mode ignores the config file
 * entirely (default state_dir "./gateway-state" leaks into the source tree)
 * and defers registry loading to replay-archive extraction, leaving the
 * dashboard table empty.
 *
 * Then starts the Vite HMR dev server on port 5173, proxying all API/WS
 * calls to the gateway on port 8001.
 *
 * Env var overrides:
 *   SGRN_BUILD_DIR   path to CMake build dir           (default: <root>/.build/linux-static-release)
 *   SGRN_GW_CONFIG   gateway.json path                 (default: dev/gateway.json)
 *   SGRN_SCHEMA      schema.scl path                   (default: dev/schema.scl)
 *   SGRN_DEV_PORT    gateway HTTP port                 (display/info only — the listen port comes
 *                                                      from SGRN_GW_CONFIG's northbound block)
 *
 * To use a shared simulation instead of the dev config:
 *   SGRN_GW_CONFIG=sgrn/lib/gateway/simulations/pump/gateway.json \
 *   SGRN_SCHEMA=sgrn/lib/gateway/simulations/pump/schema.scl \
 *   bun run dev
 */

import path from "path";

const PRESET_BUILD_DIR: string = ".build/linux-static-release";
// ── Paths ─────────────────────────────────────────────────────────────────────
const ROOT = path.resolve(import.meta.dir, "../../..");
const BUILD_DIR =
  process.env.SGRN_BUILD_DIR ?? path.join(ROOT, PRESET_BUILD_DIR);
const DEV_PORT = process.env.SGRN_DEV_PORT ?? "8001";
const BINARY = path.join(BUILD_DIR, "sgrn/apps/gateway/gateway");

const DEV_DIR = path.join(import.meta.dir, "dev");
const GW_CONFIG =
  process.env.SGRN_GW_CONFIG ?? path.join(DEV_DIR, "gateway.json");
const SCHEMA = process.env.SGRN_SCHEMA ?? path.join(DEV_DIR, "schema.scl");

// ── Validate binary ───────────────────────────────────────────────────────────
if (!(await Bun.file(BINARY).exists())) {
  console.error(`\n  Binary not found: ${BINARY}`);
  console.error(
    `    Build it first:  cmake --build ${BUILD_DIR} --target gateway\n`,
  );
  process.exit(1);
}

// ── Launch gateway ────────────────────────────────────────────────────────────
const args = [BINARY, "-c", GW_CONFIG, "-s", SCHEMA];

console.log(`\n▶  gateway  (dev port ${DEV_PORT} from dev/gateway.json)`);
console.log(`   binary : ${BINARY}`);
console.log(`   config : ${GW_CONFIG}`);
console.log(`   schema : ${SCHEMA}\n`);

const backend = Bun.spawn(args, {
  stdout: "inherit",
  stderr: "inherit",
  env: { ...(process.env as Record<string, string>) },
});

await Bun.sleep(1500);

if (backend.exitCode !== null) {
  console.error(`\n gateway exited immediately (code ${backend.exitCode}).`);
  console.error(
    `    Check the config or run the binary manually to see the error.\n`,
  );
  process.exit(1);
}

// ── Launch Vite ───────────────────────────────────────────────────────────────
console.log(
  `▶  Vite dev server → http://localhost:5173  (proxying API/WS → :${DEV_PORT})\n`,
);

const vite = Bun.spawn(["bun", "x", "vite"], {
  cwd: import.meta.dir,
  stdout: "inherit",
  stderr: "inherit",
  env: { ...(process.env as Record<string, string>) },
});

// ── Cleanup ───────────────────────────────────────────────────────────────────
function shutdown() {
  console.log("\n⏹  Shutting down dev stack…");
  vite.kill();
  backend.kill();
  process.exit(0);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

await vite.exited;
backend.kill();
