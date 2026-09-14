# sgrn_datastore

Self-contained persistence/API platform: HTTP REST-like API over
PostgreSQL/TimescaleDB, object storage (Garage, S3-compatible), Redis and an embedded
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
| `--config-systemd` | Systemd unit setup |

First-time deployment needs PostgreSQL 15+, Garage v2.x, Redis 7+
and Nginx 1.24+ (all in the SGRN Micromamba env, except the `garage`
binary); run `--help` for the full
printed guide.

## Dependencies

Links `sgrn_datastore_lib` (whole-archive in static builds so Drogon's
global-constructor route registration survives `--as-needed`), with Drogon,
Trantor, fmt, jsoncpp, OpenSSL, PostgreSQL, zstd, Redis and AWS SDKs via
the library. Builds with the `sgrn_pch_net` precompiled header; dashboard,
SQL and config assets are embedded at compile time.
