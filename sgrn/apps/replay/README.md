# sgrn_replay

History archive replayer: drives the gateway (or a headless HTTP+WebSocket
subset) from a recorded `.bin.zst`/`.jsonl.zst` archive instead of a live
PLC. Engine is `GatewayReplayer`; this directory holds only the CLI entry
point.

## Role

Two modes:

- **Full** (`-c gateway.json -a ARCHIVE`): boots the configured gateway
  stack and replays frames into twin memory at real or scaled speed.
- **Headless** (`-a ARCHIVE -s SCHEMA`, no config): starts HTTP
  (default :8080) with the WebSocket endpoint at `/ws` on the same
  listener — no S7/OPC-UA/Modbus,
  no persistence — and auto-opens the embedded dashboard. This is the
  zero-config development path.

Supports `--speed` multiplier, `--loop`, `--no-delay` (max speed) and
`--gui`. Runs until the archive ends or SIGINT/SIGTERM.

## Usage

```text
sgrn_replay -c gateway.json -a ARCHIVE.bin.zst [-s SCHEMA.scl] [-r SPEED] [--loop]
sgrn_replay -a ARCHIVE.bin.zst -s SCHEMA.scl [--http-port 8080]
```

```bash
sgrn_replay -a run.bin.zst -s schema.scl -r 2.0
sgrn_replay --man   # full manual page
```

Archive is positional (`sgrn_replay ARCHIVE` works); `--help` without an
archive prints usage.

## Dependencies

Links `sgrn_gateway` (adapters + embedded dashboard), `sgrn_gateway_twin`,
`sgrn::scl`, `sgrn_utils`, zstd, cxxopts, RapidJSON and fmt. Compiles
`sgrn/lib/gateway/src/gateway.cpp` directly (passive-replay wiring).
