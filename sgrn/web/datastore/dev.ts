#!/usr/bin/env bun
/**
 * sgrn/web/datastore/dev.ts
 *
 * `bun run dev` entry point for the datastore dashboard.
 *
 * On first run (dev database absent):
 *   1. Creates `sgrn_dev` PostgreSQL database
 *   2. Applies the full schema (same SQL sequence as --init-db, always current)
 *   3. Runs dev/seed.sql (extra test users / data)
 *
 * On subsequent runs the database check is skipped (idempotent).
 *
 * Then starts sgrn_datastore (port 5001) and the Vite HMR server (port 5173).
 * Kills both cleanly on Ctrl-C.
 *
 * The dev database is DISPOSABLE: it is dropped on every shutdown (Ctrl-C or
 * Vite exit) so the next run re-initializes from the current source SQL.
 * Set SGRN_KEEP_DEV_DB=1 to keep it between runs.
 *
 * An EPHEMERAL Garage (S3) instance backs uploads/VFS testing: single node,
 * own port set, random credentials per run, data in a temp dir wiped on
 * shutdown. Set SGRN_NO_GARAGE=1 to disable (falls back to whatever endpoint
 * the config points at).
 *
 * Env var overrides:
 *   SGRN_BUILD_DIR             path to CMake build dir        (default: <root>/.build/linux-static-release)
 *   SGRN_DEV_CONFIG            path to Drogon dev JSON        (default: sgrn/lib/datastore/configs/sgrn.dev.json)
 *   SGRN_DEV_PORT              backend listen port            (default: 5001)
 *   SGRN_DEV_DB                dev PostgreSQL database name   (default: sgrn_dev)
 *   SGRN_KEEP_DEV_DB           keep dev DB between runs       (default: drop on exit)
 *   SGRN_NO_GARAGE             skip the ephemeral Garage       (default: start one)
 *   SGRN_DEV_S3_PORT           ephemeral Garage base S3 port  (default: 3900; RPC/admin shift up)
 *   POSTGRES_HOST              PostgreSQL host                (default: 127.0.0.1)
 *   POSTGRES_PORT              PostgreSQL port                (default: 5432)
 *   POSTGRES_SUPERUSER         superuser for DB creation      (default: postgres)
 *   POSTGRES_SUPERUSER_PASSWORD superuser password (optional)
 *   POSTGRES_USER              app role name                  (default: sgrn_datastore)
 *   POSTGRES_PASSWORD          app role password              (default: sgrn_datastore)
 *   JWT_SECRET                 JWT signing secret             (default: dev-jwt-secret-not-for-production)
 */

import path from "path";
import { mkdtemp, writeFile, unlink, mkdir, rm } from "fs/promises";
import { tmpdir, userInfo } from "os";
import { randomBytes } from "crypto";

// ── Paths ─────────────────────────────────────────────────────────────────────
const ROOT = path.resolve(import.meta.dir, "../../..");
const BUILD_DIR = process.env.SGRN_BUILD_DIR ?? path.join(ROOT, ".build/linux-static-release");
const DEV_CONFIG = process.env.SGRN_DEV_CONFIG ?? path.join(ROOT, "sgrn/lib/datastore/configs/sgrn.dev.json");
const DEV_PORT = process.env.SGRN_DEV_PORT ?? "5001";
const BINARY = path.join(BUILD_DIR, "sgrn/apps/datastore/sgrn_datastore");

// ── Database config ───────────────────────────────────────────────────────────
// Superuser: scripts/db.py runs initdb WITHOUT -U, so the cluster superuser is
// the OS user (e.g. "odahim") — not "postgres". Override with POSTGRES_SUPERUSER
// for clusters initialized differently.
const PG_HOST = process.env.POSTGRES_HOST ?? "127.0.0.1";
const PG_PORT = process.env.POSTGRES_PORT ?? "5432";
const PG_SUPERUSER = process.env.POSTGRES_SUPERUSER ?? userInfo().username;
const PG_SU_PASS = process.env.POSTGRES_SUPERUSER_PASSWORD ?? "";
const DEV_DB = process.env.SGRN_DEV_DB ?? "sgrn_dev";
const APP_USER = process.env.POSTGRES_USER ?? "sgrn_datastore";
const APP_PASS = process.env.POSTGRES_PASSWORD ?? "sgrn_datastore";
const JWT_SECRET = process.env.JWT_SECRET ?? "dev-jwt-secret-not-for-production";
// Dev database is disposable: dropped on every shutdown so the next run
// re-initializes from source SQL (clean slate). SGRN_KEEP_DEV_DB=1 to opt out.
const KEEP_DEV_DB = process.env.SGRN_KEEP_DEV_DB === "1";

// SQL source directories
const PG_SRC = path.join(ROOT, "sgrn/lib/datastore/postgres");
const DEV_SEED = path.join(ROOT, "sgrn/web/datastore/dev/seed.sql");

// ── Validate binary ───────────────────────────────────────────────────────────
if (!(await Bun.file(BINARY).exists())) {
    console.error(`\n❌  Binary not found: ${BINARY}`);
    console.error(`    Build it first:  cmake --build ${BUILD_DIR} --target sgrn_datastore\n`);
    process.exit(1);
}

// ── DB helpers ────────────────────────────────────────────────────────────────
function psqlEnv(): Record<string, string> {
    const e: Record<string, string> = { ...(process.env as Record<string, string>) };
    if (PG_SU_PASS) e["PGPASSWORD"] = PG_SU_PASS;
    return e;
}

async function runPsql(sqlFile: string, database = "postgres", cwd?: string): Promise<boolean> {
    const proc = Bun.spawn(
        ["psql", "-h", PG_HOST, "-p", PG_PORT, "-U", PG_SUPERUSER, "-d", database, "-v", "ON_ERROR_STOP=1", "-f", sqlFile],
        { stdout: "inherit", stderr: "inherit", env: psqlEnv(), ...(cwd ? { cwd } : {}) },
    );
    const code = await proc.exited;
    return code === 0;
}

async function dbExists(name: string): Promise<boolean> {
    const proc = Bun.spawn(
        [
            "psql",
            "-h",
            PG_HOST,
            "-p",
            PG_PORT,
            "-U",
            PG_SUPERUSER,
            "-d",
            "postgres",
            "-tAc",
            `SELECT 1 FROM pg_database WHERE datname='${name}'`,
        ],
        { stdout: "pipe", stderr: "pipe", env: psqlEnv() },
    );
    const text = await new Response(proc.stdout).text();
    await proc.exited;
    return text.trim() === "1";
}

// ── Dev database lifecycle ────────────────────────────────────────────────────
//
// The dev database is disposable: owned by this dev environment, dropped on
// every shutdown so the next `bun run dev` re-initializes it from source SQL.

/**
 * Recursively inline a SQL script: \i / \ir targets are expanded in place
 * (kept relative to their including file, matching the tree layout) and
 * every file's ${...} placeholders are substituted on the way. `seen` de-dupes
 * files pulled in from multiple branches. psql then executes one flat script,
 * so include-dir resolution quirks between psql builds cannot bite.
 */
async function buildSql(sqlPath: string, seen: Set<string>): Promise<string> {
    if (seen.has(sqlPath)) {
        return `-- (${sqlPath.replace(PG_SRC, "postgres")} already inlined\n)`;
    }
    seen.add(sqlPath);
    const rendered = renderConfig(await Bun.file(sqlPath).text());
    const chunks: string[] = [`-- ================ ${sqlPath.replace(ROOT, ".")} ================`];
    for (const line of rendered.split("\n")) {
        const m = line.match(/^\s*\\i(?:r)?\s+(\S+)\s*$/);
        if (!m) {
            chunks.push(line);
            continue;
        }
        const target = path.resolve(path.dirname(sqlPath), m[1]);
        chunks.push(await buildSql(target, seen));
    }
    return chunks.join("\n");
}

/**
 * Render ${VAR} placeholders (both SQL and JSON sources use this style).
 * Injects the dev-stack credentials; unknown vars fall back to the process
 * environment (GARAGE_* keys for the Dev S3). Any placeholder that survives
 * rendering is reported so misconfigured sources fail loudly here instead of
 * at connection time ("role \"${POSTGRES_USER}\" does not exist").
 */
function renderConfig(raw: string): string {
    const values: Record<string, string> = {
        POSTGRES_DB: DEV_DB,
        POSTGRES_USER: APP_USER,
        POSTGRES_PASSWORD: APP_PASS,
        JWT_SECRET: JWT_SECRET,
        GARAGE_ACCESS_KEY: process.env.GARAGE_ACCESS_KEY ?? "",
        GARAGE_SECRET_KEY: process.env.GARAGE_SECRET_KEY ?? "",
    };
    const rendered = raw.replace(/\$\{([A-Za-z_][A-Za-z0-9_]*)\}/g, (match, name) => {
        const value = values[name] ?? process.env[name];
        return value !== undefined ? value : match;
    });
    const leftovers = new Set(rendered.match(/\$\{[A-Za-z_][A-Za-z0-9_]*\}/g) ?? []);
    if (leftovers.size > 0) {
        console.warn(`⚠️  Unresolved placeholders: ${[...leftovers].join(", ")}`);
    }
    return rendered;
}

async function initDevDatabase(): Promise<void> {
    // Disposable mode (default): ALWAYS re-init. init.sql force-drops and
    // recreates, so half-initialized leftovers from a crashed run (CREATE
    // DATABASE succeeds before later statements can fail) can never poison
    // a session. In keep mode we skip when the DB already exists (fast path).
    if (KEEP_DEV_DB) {
        console.log(`\n🗄️  Checking dev database '${DEV_DB}'...`);
        if (await dbExists(DEV_DB)) {
            console.log(`   ✓ Already exists — skipping init (SGRN_KEEP_DEV_DB=1)\n`);
            return;
        }
        console.log(`   ▶ Not found — initialising from source SQL...\n`);
    } else {
        console.log(`\n🗄️  Initialising (disposable) dev database '${DEV_DB}'...\n`);
    }

    // Recursively inline the SQL include tree (postgres/init.sql -> schemas,
    // views, functions, seeds -> their own \i / \ir siblings) into ONE flat
    // script, substituting ${...} placeholders at every level. Deliberately
    // NOT relying on psql \i semantics: this psql build resolves bare \i
    // against the process CWD, which breaks nested includes. A flat script
    // works on any psql build.
    const stage = await mkdtemp(path.join(tmpdir(), "sgrn-dev-"));
    STAGE_DIRS.push(stage);
    const bootstrap =
        "\\set ON_ERROR_STOP true\n" +
        (await buildSql(path.join(PG_SRC, "init.sql"), new Set())) +
        "\n" +
        (await buildSql(DEV_SEED, new Set()));

    const sqlPath = path.join(stage, "bootstrap.sql");
    await writeFile(sqlPath, bootstrap, "utf8");

    const ok = await runPsql(sqlPath, "postgres", stage);
    await unlink(sqlPath).catch(() => {});

    if (!ok) {
        console.error(`\n❌  Database initialisation failed.`);
        console.error(`    Fix the error above, then re-run bun run dev.`);
        console.error(`    (The '${DEV_DB}' database was NOT created, so next run will retry.)\n`);
        process.exit(1);
    }

    console.log(`   ✓ Dev database '${DEV_DB}' ready\n`);
}

// Drop the dev database on shutdown (SGRN_KEEP_DEV_DB=1 keeps it). `WITH
// (FORCE)` terminates lingering backend connections (PG >= 13) — needed
// because sgrn_datastore's shutdown may lag ours by a beat.
async function dropDevDatabase(): Promise<void> {
    if (KEEP_DEV_DB) {
        console.log(` Keeping dev database '${DEV_DB}' (SGRN_KEEP_DEV_DB=1)\n`);
        return;
    }
    console.log(`🗑️  Dropping dev database '${DEV_DB}'...`);
    const proc = Bun.spawn(
        [
            "psql",
            "-h",
            PG_HOST,
            "-p",
            PG_PORT,
            "-U",
            PG_SUPERUSER,
            "-d",
            "postgres",
            "-v",
            "ON_ERROR_STOP=1",
            "-c",
            `DROP DATABASE IF EXISTS ${DEV_DB} WITH (FORCE);`,
        ],
        { stdout: "inherit", stderr: "inherit", env: psqlEnv() },
    );
    const code = await proc.exited;
    if (code !== 0) {
        console.warn(`   (drop failed — '${DEV_DB}' left in place; next run will init from it)\n`);
    } else {
        console.log(`   ✓ '${DEV_DB}' dropped — next run re-initializes\n`);
    }
}

// ── Ephemeral Garage (S3) ─────────────────────────────────────────────────────
//
// A single-node Garage server backing upload/VFS UI development. Fully
// ephemeral: temp metadata/data dirs, random credentials per run, ports
// auto-shifted when occupied, wiped on shutdown. Layout mirrors
// documentation/datastore/storage.md and scripts/deploy.py provisioning.
// SGRN_NO_GARAGE=1 disables; SGRN_DEV_S3_PORT sets the base S3 port.

const NO_GARAGE = process.env.SGRN_NO_GARAGE === "1";

let garage_proc: Bun.Subprocess | null = null;
let garage_dir: string | null = null;
let garage_s3_endpoint = "http://127.0.0.1:3900"; // matches sgrn.dev.json default
// Temp staging dirs (bootstrap SQL / rendered config) wiped on shutdown.
const STAGE_DIRS: string[] = [];

// Wipe every staging dir this session created (best effort).
async function cleanStaging(): Promise<void> {
    while (STAGE_DIRS.length > 0) {
        const dir = STAGE_DIRS.pop()!;
        await rm(dir, { recursive: true, force: true }).catch(() => {});
    }
}

async function isPortFree(port: number): Promise<boolean> {
    try {
        const probe = Bun.listen({
            port,
            hostname: "127.0.0.1",
            socket: { data() {}, close() {}, error() {}, open() {} },
        });
        probe.stop(true);
        return true;
    } catch {
        return false;
    }
}

function garageCli(cfgPath: string, args: string[]): Promise<{ ok: boolean; out: string }> {
    return new Promise((resolve) => {
        const proc = Bun.spawn(["garage", "-c", cfgPath, ...args], {
            stdout: "pipe",
            stderr: "pipe",
        });
        Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text()]).then(([out, err]) =>
            proc.exited.then((code) => resolve({ ok: code === 0, out: out + err })),
        );
    });
}

async function startEphemeralGarage(): Promise<void> {
    if (NO_GARAGE) {
        console.log("🪣  Garage disabled (SGRN_NO_GARAGE=1) — uploads/VFS degraded\n");
        return;
    }
    const exe = Bun.which("garage");
    if (!exe) {
        console.error("\n❌  `garage` binary not found on PATH (need v2.x, S3-compatible object store).");
        console.error("    Install it or set SGRN_NO_GARAGE=1 to run degraded without S3.\n");
        process.exit(1);
    }

    // Find a free (s3, rpc, admin) port triple, probing upward from 3900 so
    // a concurrently running production Garage never blocks the dev stack.
    const S3_PORT = Number(process.env.SGRN_DEV_S3_PORT ?? 3900);
    let base = 0;
    for (let i = 0; i < 50; i++) {
        if (
            await Promise.all([isPortFree(S3_PORT + i), isPortFree(S3_PORT + i + 1), isPortFree(S3_PORT + i + 3)]).then((r) =>
                r.every(Boolean),
            )
        ) {
            base = i;
            break;
        }
    }
    if (base > 0) {
        console.warn(
            `⚠️  Default Garage ports busy — shifted to ${S3_PORT + base}/${S3_PORT + base + 1}/${S3_PORT + base + 3} (S3/RPC/admin)`,
        );
        garage_s3_endpoint = `http://127.0.0.1:${S3_PORT + base}`;
    }
    const S3P = S3_PORT + base,
        RPCP = S3P + 1,
        ADMP = S3P + 3;

    // ── Stage data dirs + config ─────────────────────────────────────────────
    garage_dir = await mkdtemp(path.join(tmpdir(), "sgrn-dev-garage-"));
    await mkdir(path.join(garage_dir, "meta"), { recursive: true });
    await mkdir(path.join(garage_dir, "data"), { recursive: true });

    const ACCESS = randomBytes(12).toString("hex");
    const SECRET = randomBytes(20).toString("hex");
    // Garage requires 32 bytes (64 hex chars) for rpc_secret/admin_token.
    const TOKEN = randomBytes(32).toString("hex");
    const NODE = randomBytes(32).toString("hex");

    const toml = `
metadata_dir = "${path.join(garage_dir, "meta")}"
data_dir     = "${path.join(garage_dir, "data")}"
db_engine = "lmdb"
replication_factor = 1
consistency_mode = "consistent"

rpc_bind_addr = "127.0.0.1:${RPCP}"
rpc_public_addr = "127.0.0.1:${RPCP}"
rpc_secret = "${NODE}"
bootstrap_peers = []

[s3_api]
s3_region = "us-east-1"
api_bind_addr = "127.0.0.1:${S3P}"

[admin]
api_bind_addr = "127.0.0.1:${ADMP}"
admin_token = "${TOKEN}"
`;
    const cfgPath = path.join(garage_dir, "garage.toml");
    await writeFile(cfgPath, toml, "utf8");

    // ── Start server + wait for admin /health ─────────────────────────────────
    console.log(`🪣  garage (ephemeral, S3 :${S3P}, admin :${ADMP})`);
    console.log(`   binary : ${exe}`);
    console.log(`   data   : ${garage_dir}\n`);

    garage_proc = Bun.spawn([exe, "-c", cfgPath, "server"], { stdout: "inherit", stderr: "inherit" });

    let healthy = false;
    for (let i = 0; i < 40; i++) {
        await Bun.sleep(250);
        if (garage_proc.exitCode !== null) break;
        try {
            // Any HTTP answer means the admin listener is up. Note: /health
            // reports 500 ("Quorum not available") until the layout is
            // applied — which happens right after this — so do NOT gate on 200.
            const res = await fetch(`http://127.0.0.1:${ADMP}/health`, { signal: AbortSignal.timeout(1000) });
            if (res.status > 0) {
                healthy = true;
                break;
            }
        } catch {
            /* not up yet */
        }
    }
    if (!healthy) {
        console.error("\n❌  Garage did not become healthy (admin /health) — aborting dev stack.\n");
        garage_proc.kill();
        process.exit(1);
    }

    // ── Single-node layout + key/bucket provisioning ─────────────────────────
    // `node id` prints "<full-id>@<rpc-addr>" (status only shows short ids);
    // layout assign needs the full id.
    const id = await garageCli(cfgPath, ["node", "id"]).then((r) => r.out.trim().split(/\s+/)[0].split("@")[0]);
    const steps: Array<[string, string[]]> = [
        ["layout assign", ["layout", "assign", "-z", "dc1", "-c", "10G", id]],
        ["layout apply", ["layout", "apply", "--version", "1"]],
        ["key import", ["key", "import", ACCESS, SECRET, "-n", "sgrn-dev-datastore", "--yes"]],
        ["bucket create", ["bucket", "create", "sgrn-uploads"]],
        ["bucket allow", ["bucket", "allow", "--read", "--write", "--owner", "sgrn-uploads", "--key", "sgrn-dev-datastore"]],
    ];
    for (const [label, args] of steps) {
        const r = await garageCli(cfgPath, args);
        if (!r.ok) {
            console.error(`\n❌  garage ${label} failed:\n    ${r.out.trim().split("\n").join("\n    ")}\n`);
            garage_proc.kill();
            process.exit(1);
        }
    }

    // Publish credentials for the rendered datastore config (renderConfig
    // falls back to process.env for GARAGE_*).
    process.env.GARAGE_ACCESS_KEY = ACCESS;
    process.env.GARAGE_SECRET_KEY = SECRET;
    console.log(`   ✓ healthy — bucket 'sgrn-uploads' ready (access key ${ACCESS.slice(0, 6)}…)\n`);
}

// Wipe the ephemeral Garage (server killed already or hanging).
async function stopEphemeralGarage(): Promise<void> {
    garage_proc?.kill();
    garage_proc = null;
    if (garage_dir) {
        await rm(garage_dir, { recursive: true, force: true }).catch(() => {});
        console.log("🧹  garage ephemeral data wiped\n");
        garage_dir = null;
    }
}

// ── Run DB init ───────────────────────────────────────────────────────────────
await initDevDatabase();

// ── Start ephemeral Garage (S3) ───────────────────────────────────────────────
// Must precede the config render: it publishes GARAGE_ACCESS_KEY/SECRET via
// process.env, which renderConfig() picks up.
await startEphemeralGarage();

// ── Backend config: render ${...} placeholders into a staging copy ───────────
// The binary does NOT expand ${VAR} in gateway-style configs (evidenced by
// 'FATAL: role "${POSTGRES_USER}" does not exist'), so env injection alone is
// insufficient. Render the JSON here and hand the staged copy to the binary.
{
    const raw = await Bun.file(DEV_CONFIG).text();
    let rendered = renderConfig(raw);
    // Ephemeral Garage may live on shifted ports — retarget the endpoint.
    if (rendered.includes("http://127.0.0.1:3900") && garage_s3_endpoint !== "http://127.0.0.1:3900") {
        rendered = rendered.replaceAll("http://127.0.0.1:3900", garage_s3_endpoint);
    }
    // Port pre-flight: never die on 'Address already in use'. A stale dev
    // instance (or anything else) owning DEV_PORT bumps us to a free port.
    // The rendered config listens there, and SGRN_DEV_PORT is exported so the
    // spawned Vite proxy (vite.config.ts) targets the same port.
    let port = Number(DEV_PORT);
    for (let i = 0; i < 10 && !(await isPortFree(port)); i++) port++;
    if (port !== Number(DEV_PORT)) {
        console.warn(`⚠️  Port ${DEV_PORT} busy — datastore moved to :${port} (Vite proxy follows)`);
        process.env.SGRN_DEV_PORT = String(port);
    }
    if (port !== 5001) {
        const cfg = JSON.parse(rendered);
        cfg.listeners = cfg.listeners ?? [];
        if (cfg.listeners.length > 0) cfg.listeners[0].port = port;
        rendered = JSON.stringify(cfg, null, 2);
    }
    const cfgStage = await mkdtemp(path.join(tmpdir(), "sgrn-dev-"));
    STAGE_DIRS.push(cfgStage);
    var RENDERED_CONFIG = path.join(cfgStage, "sgrn.dev.rendered.json");
    await writeFile(RENDERED_CONFIG, rendered, "utf8");
}

// ── Backend env (same values as the rendered config, for code paths that
// -- read the environment directly) ───────────────────────────────────────────
const backendEnv: Record<string, string> = {
    ...(process.env as Record<string, string>),
    POSTGRES_DB: DEV_DB,
    POSTGRES_USER: APP_USER,
    POSTGRES_PASSWORD: APP_PASS,
    JWT_SECRET: JWT_SECRET,
};

// ── Launch backend ────────────────────────────────────────────────────────────
console.log(`▶  sgrn_datastore  (dev port ${DEV_PORT}, db=${DEV_DB})`);
console.log(`   binary : ${BINARY}`);
console.log(`   config : ${DEV_CONFIG}`);
console.log(`   staged : ${RENDERED_CONFIG}\n`);

const backend = Bun.spawn([BINARY, RENDERED_CONFIG], {
    stdout: "inherit",
    stderr: "inherit",
    env: backendEnv,
});

await Bun.sleep(1500);

if (backend.exitCode !== null) {
    console.error(`\n❌  sgrn_datastore exited immediately (code ${backend.exitCode}).`);
    console.error(`    Check the config or run the binary manually to see the error.\n`);
    await stopEphemeralGarage();
    await cleanStaging();
    process.exit(1);
}

// ── Launch Vite ───────────────────────────────────────────────────────────────
console.log(`▶  Vite dev server → http://localhost:5173  (proxying /api → :${DEV_PORT})\n`);

const vite = Bun.spawn(["bun", "x", "vite"], {
    cwd: import.meta.dir,
    stdout: "inherit",
    stderr: "inherit",
    env: { ...(process.env as Record<string, string>) },
});

// ── Cleanup ───────────────────────────────────────────────────────────────────
let shutting_down = false;
async function shutdown() {
    if (shutting_down) return;
    shutting_down = true;
    console.log("\n⏹  Shutting down dev stack…");
    vite.kill();
    backend.kill();
    await dropDevDatabase();
    await stopEphemeralGarage();
    await cleanStaging();
    process.exit(0);
}

process.on("SIGINT", () => void shutdown());
process.on("SIGTERM", () => void shutdown());

await vite.exited;
backend.kill();
await dropDevDatabase();
await stopEphemeralGarage();
await cleanStaging();
process.exit(0);
