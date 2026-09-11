#pragma once
#include <fmt/core.h>
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
    --generate-config   Generate environment file, SSL certs, configs, and
                        systemd service templates in the operation directory.
    --init-db           Initialize/recreate the PostgreSQL database schema.
    --init              Execute config generation followed by database init.
    [config]            Path to sgrn.json (overrides default search order).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  FIRST-TIME DEPLOYMENT GUIDE
  ────────────────────────────

  Prerequisites — the following must be on $PATH (or in SGRN conda env):
    • postgres / pg_ctl          (PostgreSQL 15+)
    • postgrest                  (PostgREST 11+)
    • minio                      (MinIO latest)
    • redis-server               (Redis 7+)
    • nginx                      (Nginx 1.24+)

  These are all available in the SGRN Micromamba environment:
    micromamba activate SGRN

  ── Step 1: Generate default configurations ──────────────────────────

    {bin} --generate-config

    → Creates:  ~/.local/share/sgrn/.env
    → Generates self-signed SSL certs
    → Extracts config and systemd templates to ~/.local/share/sgrn/

  ── Step 2: Fill in your secrets ─────────────────────────────────────

    nano ~/.local/share/sgrn/.env

    Required fields to change (all marked "change_me"):
      POSTGRES_PASSWORD   — password for the sgrn_datastore DB role
      JWT_SECRET          — HS256 secret (≥ 32 chars); shared by
                            PostgREST and the Datastore API
      MINIO_ROOT_PASSWORD — MinIO object storage admin password

  ── Step 3: Run database initialization ──────────────────────────────

    {bin} --init-db

    → Drops and recreates the `sgrn` PostgreSQL database
    → Applies the embedded, Zstd-compressed schema (roles + tables)

  ── Step 4: Deploy configurations & services (requires sudo) ─────────

    sudo python3 ~/.local/share/sgrn/configure_systemd.py

    → Copies service files to /etc/systemd/system/
    → Copies Nginx configurations to the deployment environment
    → Reloads systemd daemon and restarts services

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
