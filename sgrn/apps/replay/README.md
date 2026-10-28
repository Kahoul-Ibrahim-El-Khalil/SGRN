# sgrn_replay

History archive replayer: drives the gateway (or a headless HTTP+WebSocket
subset) from a recorded `.bin.zst`/`.jsonl.zst` archive, or directly from
a discrete columnar binary dataset (`.sgrnb`), into the digital twin memory
instead of requiring a live PLC. Engine is `GatewayReplayer`; this directory
holds only the CLI entry point.

## Role

Two modes:

- **Full** (`-c gateway.json -a ARCHIVE`): boots the configured gateway
  stack and replays frames into twin memory at real or scaled speed.
- **Headless** (`-a ARCHIVE -s SCHEMA`, no config): starts HTTP
  (default :8080) with the WebSocket endpoint at `/ws` on the same
  listener — no S7/OPC-UA/Modbus,
  no persistence — and auto-opens the embedded dashboard. This is the
  zero-config development path.

Supports:
- **Formats**: `.bin.zst`, `.jsonl.zst`, and native `.sgrnb` (columnar datasets).
- **Synthetic Simulation**: Replay trained ML model predictions back into twin memory.
- **Pacing**: `--speed` multiplier, `--loop`, `--no-delay` (max speed).
- **Automation**: `--no-gui` to suppress automatic browser launch during automated integration tests and CI.

## Usage

```text
sgrn_replay -c gateway.json -a ARCHIVE [-s SCHEMA.scl] [-r SPEED] [--loop]
sgrn_replay -a ARCHIVE -s SCHEMA.scl [--http-port 8080] [--no-delay] [--no-gui]
```

```bash
# Replay live telemetry archive at 2x speed:
sgrn_replay -a run.bin.zst -s schema.scl -r 2.0

# Replay columnar ML dataset / synthetic model predictions at max speed without browser popup:
sgrn_replay -a dataset.sgrnb -s schema.scl --no-delay --no-gui --http-port 18080

# Manual page
sgrn_replay --man
```

Archive is positional (`sgrn_replay ARCHIVE` works); `--help` without an
archive prints usage.

## Dependencies

Links `sgrn_ml` (`sgrn::ml::ColumnarDatasetReader`), `sgrn_gateway` (adapters +
embedded dashboard), `sgrn_gateway_twin`, `sgrn::scl`, `sgrn_utils`, zstd,
cxxopts, RapidJSON and fmt. Compiles `sgrn/lib/gateway/src/gateway.cpp` directly
(passive-replay wiring).
