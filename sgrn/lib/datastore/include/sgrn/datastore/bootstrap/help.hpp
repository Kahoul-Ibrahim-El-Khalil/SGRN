#pragma once
#include <fmt/core.h>
#include <stdexcept>
#include <string>
namespace sgrn::datastore::bootstrap
{

constexpr const char help_message[] =
    R"(
╔══════════════════════════════════════════════════════════════════════╗
║               SGRN Datastore — Self-Contained Platform               ║
╚══════════════════════════════════════════════════════════════════════╝

  Usage:
    {bin} [OPTION] [config_path]

  Options:
    --help              Show this message and exit.
    --generate-config   Step 1: directories, .env, SSL certs, configs and
                        systemd templates in the operation directory.
    --init-db           Step 2: drop/recreate the PostgreSQL database and
                        apply the embedded schema.
    --init              Steps 1+2 in one go (still stops for secret editing).
    --config-systemd    Step 3 (root only): install systemd units + nginx
                        configs, remove obsolete units, daemon-reload,
                        enable and restart changed services.
    [config]            Path to sgrn.json (overrides default search order).

 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  FIRST-TIME DEPLOYMENT GUIDE — run in order
  ──────────────────────────────────────────

  Prerequisites — the following must be on $PATH (or in SGRN conda env):
    • postgres / pg_ctl          (PostgreSQL 15+)
    • minio                      (MinIO latest)
    • redis-server               (Redis 7+)
    • nginx                      (Nginx 1.24+)

  These are all available in the SGRN Micromamba environment:
    micromamba activate SGRN

  ── Step 1: Generate default configurations (as user, no sudo, no DB) ──

    {bin} --generate-config

    → Creates:  ~/.local/share/sgrn/.env
    → Generates self-signed SSL certs
    → Extracts config and systemd templates to ~/.local/share/sgrn/

  ── Step 2: Fill in your secrets ─────────────────────────────────────

    nano ~/.local/share/sgrn/.env

    Required fields to change (all marked "change_me"):
      POSTGRES_PASSWORD   — password for the sgrn_datastore DB role
      JWT_SECRET          — HS256 secret (≥ 32 chars) for the Datastore API
      MINIO_ROOT_PASSWORD — MinIO object storage admin password

  ── Step 3: Run database initialization (as user, PostgreSQL running) ──

    {bin} --init-db

    → Drops and recreates the `sgrn` PostgreSQL database
    → Applies the embedded, Zstd-compressed schema (roles + tables)

  ── Step 4: Deploy configurations & services (requires sudo) ──────────

    sudo {bin} --config-systemd

    → Copies service files to /etc/systemd/system/
    → Copies Nginx configurations to the deployment environment
    → Removes units of retired services (e.g. PostgREST)
    → Reloads systemd daemon; enables and restarts changed services

  ── Step 5: Verify status ────────────────────────────────────────────

    sudo systemctl status sgrn.service

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  RE-INIT (changing secrets after first deployment)
  ──────────────────────────────────────────────────

    1. Edit ~/.local/share/sgrn/.env
    2. Run:  {bin} --init-db
    3. Run:  sudo systemctl restart sgrn.service

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  RUNTIME CONFIG SEARCH ORDER
  ────────────────────────────
    1. $SGRN_CONFIG_PATH environment variable
    2. /etc/sgrn/sgrn.json          (system-wide)
    3. ~/.local/share/sgrn/sgrn.json  (user, default after --init)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
)";

inline void printHelp(const char* tp_argv0) {
    fmt::print(help_message, fmt::arg("bin", tp_argv0));
}
} // namespace sgrn::datastore::bootstrap
