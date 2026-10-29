# sgrn_datastore

Self-contained persistence/API platform: HTTP REST-like API over
PostgreSQL, object storage (Garage, S3-compatible), and an embedded
observability dashboard (Drogon-based). This directory holds the entry
point; the backend library and asset pipelines live in `sgrn/lib/datastore`.

## Role

`main()` dispatches on the first argument, otherwise boots the Drogon app
(`configDrogonApp` + signal handling + dashboard assets, filters, handlers,
S3 plugin):

| Invocation | Effect |
|---|---|
| `sgrn_datastore` | Run the server (optional `sgrn.json` path arg) |
| `--help` | Print the deployment help (also covers first-time setup) |
| `--generate-config` | Env file, SSL certs, configs, systemd templates |
| `--init-db` | Initialize/recreate the PostgreSQL schema |
| `--init` | Config generation followed by DB init |
| `--config-systemd` | Systemd unit setup (root; nginx stays in the staging dir) |
| `--verify-install` | Drift/health report: staged vs deployed config, services, ports |

`--verify-install` needs no root. It fingerprints the outage classes this
deployment has actually hit — units drifted from staged templates after a
micromamba env update, a nginx config serving from env-owned paths (env
updates delete `etc/nginx/sites-enabled`, leaving nginx "active" with zero
server blocks), missing `var/tmp/nginx/*` buffer dirs (`[emerg] mkdir()` at
start), and a `garage` binary invisible to the deployed unit's PATH (datastore
then loops on its Garage readiness gate). Follow the printed repair sequence.

First-time deployment needs PostgreSQL 15+, Garage v2.x,
and Nginx 1.24+ (all in the SGRN Micromamba env, except the `garage`
binary); run `--help` for the full printed guide.

## Architecture & Optimizations

- **3-Tier Minimal Stack**: HTTP Server (Drogon + Nginx) + Database (PostgreSQL) + Object Storage (Garage).
- **SessionStore**: In-process C++ RAM cache with PostgreSQL persistence and cross-node cache invalidation via PostgreSQL `LISTEN / NOTIFY`.
- **Fast-Reject Token Security**:
  - Syntactic UUID token validation (<5ns) filtering malformed/junk requests before task allocation.
  - Active Token RAM Set & Negative Cache (~15ns) dropping external bot/scanner noise instantly in memory without hitting PostgreSQL.
- **Rate Limiting**: Thread-safe in-process sliding window (`InProcWindowStore`) evaluating requests in <1µs.

## Dependencies

Links `sgrn_datastore_lib` (whole-archive in static builds so Drogon's
global-constructor route registration survives `--as-needed`), with Drogon,
Trantor, fmt, jsoncpp, OpenSSL, PostgreSQL, zstd, and AWS SDKs via
the library. Builds with the `sgrn_pch_net` precompiled header; dashboard,
SQL and config assets are embedded at compile time.
