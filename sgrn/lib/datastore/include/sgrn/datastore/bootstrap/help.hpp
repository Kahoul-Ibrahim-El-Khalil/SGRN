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
    --init              Fully-guided first install (steps 1+2): interactive
                        secrets wizard (CSPRNG-generated values, owner-only
                        .env, admin password mastered once) + config render
                        + database initialization.
    --generate-config   Step 1 (unattended): directories, .env TEMPLATE with
                        still-placeholder secrets, SSL certs, configs and
                        systemd templates in the operation directory.
    --init-db           Step 2: drop/recreate the PostgreSQL database and
                        apply the embedded schema. Refuses placeholder
                        secrets left by an unattended --generate-config,
                        and refuses to drop a NON-EMPTY database without
                        --force (or a typed confirmation interactively).
    --rotate KEY[=VAL]  Rotate a live secret (repeatable): value resolves to
                        CSPRNG on ENTER / --non-interactive; archives .env,
                        writes an audit line, re-renders configs. Keys:
                        POSTGRES_PASSWORD JWT_SECRET GARAGE_ACCESS_KEY
                        GARAGE_SECRET_KEY GARAGE_ADMIN_TOKEN
    --non-interactive   No prompts: CSPRNG defaults for --init/--rotate (CI).
                        A non-empty rebuild target is REFUSED in this mode.
    --allow-insecure    Skip the change_me_* placeholder gate (--init-db).
    --force             Confirm a destructive rebuild of a non-empty (or
                        uninspectable) database target. Use with --init-db
                        or --init; without it those steps stop/abort when
                        the target already holds data.
    --config-systemd    Step 3 (root only): install systemd units, remove
                        obsolete units, ensure nginx runtime dirs,
                        daemon-reload, enable and restart changed services.
                        Nginx serves its site configs from the operation
                        directory directly (not copied into the micromamba
                        env — env updates must not take the site down).
    --verify-install    No root: report drift between staged templates and
                        the deployed system (units, nginx, binaries, certs,
                        temp dirs, running services, ports). Nonzero exit
                        when drift is found.
    --backup-db         Atomic PostgreSQL backup: dumps schema, roles and
                        data via pg_dump (-Fc -Z9) into ~/.local/share/sgrn/backup/
                        with verified TOC and manifest.
    --backup-garage     Atomic Garage S3 backup: drains catalog-referenced
                        objects (storage.objects) into an uncompressed ustar
                        tar archive with live ETA, progress and per-object
                        sha256 verification against the PostgreSQL catalog.
    --verify-backup [F] Cross-verify backup archive(s) against PostgreSQL:
                        reads member hashes and compares 100% against the
                        storage.objects catalog; checks pg_dump TOC.
    [config]            Path to sgrn.json (overrides default search order).

 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  FIRST-TIME DEPLOYMENT GUIDE — run in order
  ──────────────────────────────────────────

  Prerequisites — the following must be on $PATH (or in SGRN conda env):
    • postgres / pg_ctl          (PostgreSQL 15+)
    • garage                     (Garage v2.x, single-node layout)
    • nginx                      (Nginx 1.24+)

  These are all available in the SGRN Micromamba environment:
    micromamba activate SGRN

  ── Step 1: First install, fully guided (recommended) ─────────────────

    {bin} --init

    → Interactive wizard: secrets generated with the OS CSPRNG (Enter =
      accept; values land in ~/.local/share/sgrn/.env, owner-only 0600)
    → Admin account password shown ONCE, stored as a bcrypt hash only
    → Renders self-signed SSL certs, configs, systemd templates
    → Drops/recreates the PostgreSQL DB and applies the schema

    Unattended variants:
      {bin} --init --non-interactive        (CSPRNG everywhere, prints passwords once)
      {bin} --generate-config               (placeholder .env, edit by hand, then --init-db)

  ── Step 2: Verify secrets are real ───────────────────────────────────

    .env secrets are viewable any time: cat ~/.local/share/sgrn/.env
    A placeholder check runs automatically: --init-db refuses until the
    change_me_* values are replaced (wizard or --rotate), and
    --verify-install reports them as drift.

  ── Step 3: Run database initialization (as user, PostgreSQL running) ──

    {bin} --init-db        (skipped when --init already ran it)

    → Drops and recreates the `sgrn` PostgreSQL database
    → Applies the embedded, Zstd-compressed schema (roles + tables)

  ── Step 4: Deploy configurations & services (requires sudo) ──────────

    sudo {bin} --config-systemd

    → Copies service files to /etc/systemd/system/
    → Nginx site configs stay in ~/.local/share/sgrn/nginx/ (Serviced
      in place — never copied into the micromamba env, where env
      updates would wipe them)
    → Ensures nginx buffer/log/pid dirs exist under the data prefix
    → Removes units of retired services (e.g. PostgREST)
    → Reloads systemd daemon; enables and restarts changed services

  ── Step 5: Verify the deployment (no sudo needed) ───────────────────

    {bin} --verify-install

    → Reports staged-vs-deployed unit drift, nginx template health,
      binary/cert/temp-dir prerequisites, garage on the deployed unit
      PATH, active services and listening ports
    → Nonzero exit when something drifted; follow the printed repair
      sequence to reconcile

  ── Step 5: Verify status ────────────────────────────────────────────

    sudo systemctl status sgrn.service

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  RE-INIT / ROTATION (changing secrets after first deployment)
  ─────────────────────────────────────────────────

  Rotate one or more secrets in place (values are archived, audited,
  .env + rendered configs updated, restart commands printed):

    {bin} --rotate JWT_SECRET
    {bin} --rotate POSTGRES_PASSWORD=MyNewPass123 ...   (bring your own)

  Notes per key:
    POSTGRES_PASSWORD   — the DB role is ALTER-ROLEd BEFORE .env changes;
                          the destructive --init-db path is never used
    GARAGE_ACCESS_KEY / GARAGE_SECRET_KEY — prints the cluster-side
                          `garage key import` steps to run first
    GARAGE_ADMIN_TOKEN  — restart SGRN-garage after the datastore
    GARAGE_RPC_SECRET   — NOT rotatable (cluster identity material)

  Full fresh start (wipes data): edit .env, then {bin} --init-db.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  BACKUP & SYSTEM DUPLICATION (cloning between machines)
  ──────────────────────────────────────────────────────

  1. Create atomic snapshots (recommended with datastore stopped):

       {bin} --backup-db
       {bin} --backup-garage

     Artifacts land in ~/.local/share/sgrn/backup/:
       sgrn-bck-<stamp>.pg              (PostgreSQL custom dump -Fc -Z9)
       sgrn-bck-<stamp>.garage.tar      (ustar tar of chunk objects)
       *.manifest.json                  (counts, digests, provenance)

  2. Verify consistency (matches 100% of database hashes against archive):

       {bin} --verify-backup            (verifies newest .tar + .pg)
       {bin} --verify-backup path.tar   (verifies specific archive)

  3. Duplicate to a target machine:
     a. Copy ~/.local/share/sgrn/.env and backup artifacts to the new machine.
     b. Restore PostgreSQL:
          pg_restore --clean --if-exists -h $POSTGRES_HOST -p $POSTGRES_PORT \
                     -U $POSTGRES_SUPERUSER -d sgrn sgrn-bck-<stamp>.pg
     c. Restore Garage S3 objects:
          garage key import ... (or reuse keys from .env)
          garage bucket create sgrn-uploads
          extract/upload objects from sgrn-bck-<stamp>.garage.tar
     d. Run `{bin} --verify-backup` to ensure all objects match the restored DB.

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
