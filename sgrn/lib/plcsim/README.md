# sgrn_plcsim — virtual-PLC data plane

Owns schema, memory, tag table and DB I/O for one PLC's worth of memory,
plus the simulation and WAL-replay drivers. Knows nothing about wire
protocols, the script engine, or the REPL — extracted from `sgrn_s7shell`,
which remains its primary consumer (script bindings, servers, shell).

## Contents

- `runtime/` — `PlcRuntime` (schema + `PlcMemory` + tag table ownership,
  `loadSclSchema/loadJsonSchema/registerDb/registerUdt`), `PersistenceBridge`
  (record-to-WAL side).
- `simulation/` — `SimulationEngine` (parameterized synthetic plant runs).
- `replay/` — `WalReplayer` (re-drive recorded archives into memory).
- `utils/PlcSimClock` — simulated clock shared by runtime and simulation.
- `PlcTagTable` — tag model backing script `TagTable@` handles.

## Contracts

- Links `sgrn_gateway_twin`, `sgrn_gateway` (database/config surface),
  `sgrn_scl`; deliberately **not** security, adapters, AngelScript, or the
  shell — no `sgrn_s7shell` include or link anywhere in this tree.
- `g_on_schema_loaded` hook (`runtime/PlcRuntime.hpp`): invoked at the end
  of `loadSclSchema()`/`loadJsonSchema()` when set. Null by default
  (headless use); armed once by s7shell's `registerS7Shell()` next to
  `p_g_as_engine` so script-visible types re-register after reloads.
  Same (unsynchronized) contract as `p_g_as_engine`.

## Consumers

`sgrn_s7shell` (bindings, servers, commands), `sgrn/apps/s7shell` (via the
shell lib), `wal_replayer_test` and `schema_reload_hook_test`.
