# Gas Processing Simulation (Molecular Sieve Dehydration)

## Overview
This scenario simulates a natural gas dehydration unit using a 3-tower molecular sieve design. It is highly elaborate, modelling dynamic physics, thermodynamic equations, and mechanical wear over time.

## What is Simulated?
- **3-Tower State Machine**: The 3 adsorber towers cycle through Adsorption, Depressurization, Heating, Cooling, and Repressurization states dynamically.
- **Process Noise**: Random noise is injected into feed pressures, feed flows, and blower flows to emulate real-world sensor fluctuations and process instability.
- **Fouling**: The inlet coalescer filter gradually fouls over time (differential pressure increases).
- **Thermodynamics (PID)**: The regeneration gas heater employs a PID-like response curve to ramp up to the 290°C target during the heating cycle and cool down naturally during the cooling cycle.
- **Moisture Saturation**: Desiccant beds gradually saturate with moisture depending on the incoming feed flow rate. The sales gas dew point directly reflects the condition of the worst-performing online tower.
- **Fault Scenarios**:
  - `fault_code = 8`: Molecular sieve non-absorption at Hour 4; active adsorber fails to retain moisture leading to rapid dew point breakthrough.
  - `fault_code = 9`: **FLARE_BLOCKED** at Hour 3 of the fault day (Day 31). Physically models an obstructed flare header line where relief is ineffective. `flare_header_press` ramps up from 1.05 bar at ~+0.15 bar/tick; `feed_press` climbs gradually (+1.15 bar/tick), causing `scrubber_press`, `system_inlet_press`, and `system_outlet_press` to rise with dynamic lag alongside small `bed_dp` increases.
- **Emergency Shutdown (ESD)**: If the dew point spec is grossly violated, feed pressure spikes (`feed_press > 90.0` bar), or the heater runs away, the ESD trips.

## Enabled / Disabled Features
- **S7 Adapter**: **Enabled** (Port 102).
- **HTTP/WebSocket API**: **Enabled** (Port 8000; WebSocket at `/ws`).
- **Modbus Adapter**: **Disabled** in the default simulation configuration due to port 502 root binding restrictions.
- **Security**: **Strict Mode**. AngelScript policy enforced via `security.as`.
- **Persistence**: **Enabled**. Snapshots are flushed to `/tmp/sgrn-gateway-state`.

## How to Test
1. Observe the cycling state of the towers in `DB2` (`AdsorberTowers`).
2. Watch the `coalescer_dp` in `DB1` slowly rise due to fouling.
3. Observe the `heater_outlet_temp` ramping up and down in `DB3` in sync with the tower states.

For the complete recorded-history experiment, run `python3 demos/history_twin.py` from the repository root. The isolated S7Shell simulation uses its `Persistence` binding to write the WAL, then the experiment replays that archive through `sgrn_replay` and runs a prediction gateway beside it. The temporary gateway serves the connection during capture but does not record the archive. The `--fault` option adds a selected fault (`8` = sieve non-absorption, `9` = flare blocked) to the isolated recording copy, leaving this baseline simulation unchanged.

For the minimal partial-loop prototype evaluating the flare blocked fault with PyTorch conformal anomaly detection and read-only LangChain agent monitoring, see [experiments/flare/README.md](../../../../experiments/flare/README.md).

> [!TIP]
> **Forcing Values:** Because the Gateway acts as a unified hub, you can interactively force variables and override the running physics using an external **OPC-UA Client** (connecting to `opc.tcp://localhost:4840`) or by directly connecting with the **`s7shell`** in a separate terminal.

## High-Speed Operation (Simulation Time vs Wall-Clock Time)

One simulation tick advances `SIM_SECONDS_PER_TICK` (default `3600.0` = one
plant hour). `SIM_TICK_SLEEP_MS` (default `1`, `0` = unpaced) only paces
wall-clock throughput; it never changes the simulated trajectory
(`sleep(n)` waits *n milliseconds* — verified in
`sgrn/lib/core/include/sgrn/AngelScriptEngine.hpp::sleepCmd`).

### Root cause of the "streaming stops without sleep" symptom

Removing the sleep was never the direct cause. Three compounding defects made
fast runs look stalled or corrupt (all verified by code inspection and by the
measurements below):

1. **Dirty-ledger coalescing + ack-serialized publishing (primary).**
   `GatewaySync`'s publish worker debounces ~25 ms and each
   `publishDirtyBatch()` blocks on a 5 s ACK timeout, while an unpaced loop
   runs ~1500+ ticks/s. `PlcRuntime::markDirty()` merges overlapping regions
   and `takeDirty()` consumes them, so intermediate tick states are
   overwritten in memory before the worker reads them. The gateway keeps
   receiving *latest-only* snapshots (measured: 7–13 persisted lines per 200
   ticks pre-fix) — appearing "stopped" to anyone expecting per-tick updates.
2. **Gateway echo-back race (correctness).** The gateway broadcasts every
   applied write to all subscribers *including the originator* (no originator
   exclusion in `WebSocketAdapter::collectTargets`). The sim's WebSocket thread
   applies echoes via `writeFieldThroughRuntime`, clobbering newer sim state.
   Absolute-written fields self-heal next tick, but the legacy
   `DB.field.totalizer += delta` read-modify-write lost increments: measured
   200-tick true sum `169848` (no gateway) vs `97708`/`94264` connected, with
   the loss depending on round-trip timing — i.e. on the sleep interval.
3. **Unsubscribe-all meant firehose (suppression failure).** Empty subscription
   set = "send everything" (`event_filter::shouldSend`). Unsubscribing all DBs
   therefore *maximised* echo instead of stopping it (observed as
   `queued_dbs=7` at every metric tick), and stale echoes re-published during
   drain regressed the gateway twin. Fixed in `GatewaySync` (mute holding
   path, no API change) — see below.

Audited and excluded: `sleep(1)` semantics (is 1 ms); the gateway 4 KiB text
cap (this schema's JSON write is ~1.7 KiB, binary frame ~1 KiB); send-buffer
loss (failed sends re-mark dirty and retry); connection drops (2000-tick soak
stayed connected); timestamp-before-tick (fixed); `setupEnv()` ignoring its
`ws_url` argument (fixed — it used the constant).

### Architecture and trade-offs

- **Simulation and publishing are decoupled by the existing dirty ledger:**
  the script thread computes ticks; `GatewaySync`'s background worker sends.
  Queued state is bounded to one merged region per DB (~938 bytes total
  here). No script threads were added (AngelScript cannot create them).
- **Every-tick mode** (`PUBLISH_EVERY_TICK = true`, default): the script calls
  `sync.sync()` after each tick, which blocks for the gateway ACK. Every state
  is applied to the gateway twin in order (attempts/counters prove it).
  Throughput is ACK-bound (~115–165 ticks/s ≈ 115–165 sim-hours/s here —
  still ~500 000× real time). WAL granularity additionally depends on the
  gateway's `atomic_window_ms` (use `0`/`1` for per-tick lines).
- **Snapshot mode** (`false`): auto-publish only, plus a forced flush every
  `SNAPSHOT_EVERY_N_TICKS`. Intermediate states are coalesced *by design*
  (~2000+ ticks/s); the published snapshot keeps exact totalizers (script
  globals), but per-tick history for historian/regression/alarm replay
  requires every-tick mode.
- **Backpressure** is explicit, never silent: ACK-timeout batches are
  re-marked for retry; failures surface via `lastError()` (sticky — counted
  as distinct episodes) and `publish_errors`; outages trigger bounded
  reconnects (`reconnect_count`, give-up logged). Delivery stages are kept
  distinct: computed (tick) → queued (`hasDirty`/`dirtySnapshot`) → sent
  (`sync.sync()` + ACK) → received/persisted (verified out-of-band by
  `tests/gateway/gas_high_speed.py` via raw gateway memory reads and WAL
  contents — never inferred from local DB writes).
- **Determinism:** fixed `RAND_SEED` LCG (call order only, never wall time);
  physics order fixed; totalizers in script globals, absolute-written (echo
  immune); synthetic timestamps from an explicit proleptic-Gregorian calendar
  (no `setPlcTime`/libc/`TZ` dependence); `now_ms()` stays wall-clock for
  throughput metrics.

### Sync-layer changes (no API changes, full files in tree)

- `sgrn/lib/s7shell/include/sgrn/s7shell/connection/GatewaySync.hpp` /
  `sgrn/lib/s7shell/src/connection/GatewaySync.cpp`:
  (a) unsubscribe-to-empty now holds reserved non-matching subscription
  `__sgrn_receive_muted__` (plus Open-handler support) instead of falling into
  gateway firehose-echo; (b) the HTTP fallback after an ACK timeout re-reads
  the regions fresh instead of pushing up-to-5 s-stale bytes through the
  second channel (order-safe; old-gateway interop preserved).
- `sgrn/lib/s7shell/.../cmd_emit_as.cpp`,
  `sgrn/lib/s7shell/simulations/generated/s7shell_api.as`: document the
  existing (previously undeclared) `GatewaySync::sync()`.
- `experiments/flare/run_capture.py`: injected loop now stamps *after* the
  tick (was timestamp-before-tick).

### Guarantees

- Every-tick mode: every simulated state is sent, ACKed, and applied to the
  gateway twin in order. WAL per-tick lines additionally need
  `atomic_window_ms ∈ {0, 1}` on the gateway.
- Snapshot mode: only the latest state per flush interval is guaranteed; the
  latest totalizers/timestamps are exact, intermediate states are dropped by
  design.
- Identical seed + ticks ⇒ identical `[STATE]` across pacing modes and
  offline/online runs (asserted by tests).

### Validate

The `GatewaySync` fix requires rebuilding `s7shell` (header + one source
file; no gateway rebuild needed):

```bash
export PATH="$CONDA_PREFIX/micromamba/envs/SGRN/bin:$PATH"
cmake --build .build/linux-static --target s7shell -j $(nproc)
python3 tests/gateway/gas_high_speed.py            # full suite (~3 min)
python3 tests/gateway/gas_high_speed.py --ticks 60 # quick subset
python3 experiments/flare/run_capture.py --out /tmp/flare_smoke --port 18080
```

The harness prefers the fresh `.build/linux-static` binary and falls back to
the installed ones.

### Measured results (same seed; end-to-end including connect + drain)

| Config | Ticks/s | Sim-hours/s | WAL lines / 200 ticks | Twin final == sim final |
|---|---|---|---|---|
| 1 ms pacing, every-tick | ~116 | ~116 | ~62 | yes |
| Zero pacing, every-tick | ~133 | ~133 | ~60–63 | yes |
| Zero pacing, snapshot (/50) | ~840 (2000-tick soak) | ~840 | ~6–11 | yes (latest only) |
| Pre-fix, 1 ms / 0 ms | ~85 / ~95 | same | 13 / 7 | **no** (echo loss: 97708/94264 vs true 169848) |

Trajectory `[STATE]` is now byte-identical across all pacing modes and
matches the no-gateway baseline; timestamps verified monotonic across
2000-02-01, leap day 2000-02-29, 2000-03-01 and 2001-01-01.
