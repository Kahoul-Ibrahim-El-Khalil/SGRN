# s7shell

Interactive S7 shell and script runner: the soft-PLC runtime console.
Implements the REPL, script execution and `emit-as` tooling; the virtual-PLC
runtime itself lives in `sgrn_plcsim`, the script bindings in `sgrn_s7shell`.

## Role

- **Interactive**: `s7shell` drops into a REPL (`shell.run()`).
- **Scripts**: `s7shell script.as` or `s7shell run script.as` executes
  AngelScript files in order (`runScript`/`runScripts`).
- **Schema**: `-s/--schema` loads an SCL schema file or directory,
  optionally emitting a JSON registry to `-o/--output-dir`.
- **Linter tooling**: `s7shell emit-as|emit-angelscript|dump-as-api|as`
  delegates to `cmd_emit_as` (declaration-only AngelScript headers).
- `-m/--man` prints the user manual and API summary via the shell.

## Usage

```text
s7shell [run] <script.as>... [-s SCHEMA] [-o OUTDIR]
s7shell emit-as [...]        # AngelScript declaration headers
s7shell --man                # manual
```

```bash
s7shell -s schema.scl sim.as
```

Errors print to stderr with a non-zero exit.

## Use cases

- **Interactive PLC exploration**: connect to a real PLC and browse/read/write
  live values from the REPL instead of TIA Portal watch tables.
- **Edge control loops**: `void cycle() { ... }` scripts running against a
  live or virtual PLC (see Init Script below).
- **Simulation harness for integration tests**: the repo's own TS/Python
  suites spawn `s7shell simulation.as` as the process under test
  (`sgrn/lib/s7shell/simulations/` holds ready-made sims).
- **Synthetic data generation**: `s7shell simulate|record` CLI commands and
  `simulations/generate_dataset.as` produce telemetry archives for the
  dataset/ML pipeline without PLC hardware.
- **WAL replay and forensics**: `s7shell replay` re-drives recorded archives;
  `print`/schema commands inspect them.
- **Offline schema work**: load SCL schemas, emit JSON registries and
  AngelScript declaration headers without connecting anywhere.
- **Commissioning and diagnostics**: CPU info, protection, SZL lists, block
  lists, diagnostic buffers, PLC clock sync, hot/cold start and stop.

## Script API surface

AngelScript API available in the REPL, `runScript()` and `./angelscript.as`
(the `./angelscript.as` init script's globals are visible everywhere).
`s7shell --man` prints the exhaustive reference; the areas below
mirror `showHelp()`:

- **Siemens types**: `BOOL/SINT/USINT/BYTE/INT/UINT/WORD/DINT/UDINT/DWORD/
  LINT/ULINT/LWORD/REAL/LREAL/TIME/LTIME/DATE/TOD/LTOD`.
- **Virtual PLC runtime**: `PlcRuntime@ rt = PlcRuntime()` (empty),
  `PlcRuntime("plant.scl")` (schema on creation),
  `PlcRuntime("", "DB1 pump:BOOL; END_DB")` (inline SCL, for debugging);
  `rt.loadSclSchema/JsonSchema/Registry`, `registerDb/registerUdt`,
  `rt.set/get(db, "field.path")`, `getJson`, `setBit`, `hasDirty`.
  After an explicit `loadSclSchema`, DBs appear as properties
  (`rt.PrimaryCoolant.get("temp_pv")`, snake_case also works).
- **S7Server** (virtual PLC endpoint): `S7Server(rt, "0.0.0.0"[, port])`,
  `start/stop/isRunning/clientsCount/getCpuStatus`. Pattern: runtime →
  server → `S7Client("127.0.0.1", 0, 1, 102, rt)` loopback.
- **S7Client** (PLC connection): `S7Client(ip, rack, slot[, port, rt])`;
  `connect/disconnect/reconnect/ping`, `loadSclSchema/JsonSchema/Registry`,
  `registerDb/registerUdt/addUdtField`, `hasSchema/hasRegistry`,
  `read(address)/write(address, hex)`, `listSymbols/searchSymbols`,
  `db(42)/db("name")`, `tags()`, `connection()`, `schema()`,
  `lastOpOk/lastError/lastErrorCode/clearLastError`, `setConnectionType`,
  TSAP/rack-slot/port tuning passthrough.
- **DataBlock**: `get/put` (whole DB), `get(path)/put(path, val)`,
  `getReal/getInt/getBool`, `put()` + `put(path, val)` writes,
  `getRetry/putRetry`, `lastOpOk/lastOpError`, `db["field.path"]`
  (FieldProxy@), `db.path(...)` (S7PathBatch@), `toJson/diff/number/name/
  print`, `registerSize/addField` (manual authoring), `cast<HexTable>`.
- **TagTable**: `get/getReal/getInt/getBool`, `put` (immediate),
  bulk `get()/put()`, retry variants, `lastOpOk/lastOpError`,
  `path(name)`.
- **S7PathBatch** (scoped read handle): `db.path("f")`,
  `put/get/read/toJson`.
- **S7Connection** (low-level tuning): `connectWithTsap/useTsap/
  useRackSlot`, `usesTsap/localTsap/remoteTsap`, `get/setParamInt/UInt16`,
  `paramSummary`; `CONNTYPE_PG/OP/BASIC`.
- **Diagnostics**: `client.diagnostics()` → `connectionInfo/status/info/
  cpuInfo/pduInfo/isRunning/orderCode/cpInfo/protection/lastError/
  lastErrorText/diagnosticBuffer/szl/listBlocks/listBlocksOfType/blockInfo`.
- **PLC control**: `client.control()` → `hotStart/coldStart/stop/clock/
  setClock/syncClockToSystem/setPassword/clearPassword/copyRamToRom/
  compress`.
- **Low-level memory**: `client.memory()` → `readArea/writeArea`,
  `readAddress/writeAddress` (e.g. `"DB1.DBX0.0"`), `readTag/writeTag/
  tagInfo/decodeTag/listTags`, `readDB/writeDB`, `readMB/writeMB` (merkers),
  `readEB/writeEB` (inputs), `readAB/writeAB` (outputs), `readTM/writeTM`,
  `readCT/writeCT`, `saveHexToFile/loadHexFromFile`; areas `Area_DB/MK/PE/
  PA/TM/CT`, word lengths `WL_Bit/Byte/Word/DWord`.
- **PLC simulation time**: `dtl()/setPlcTime/advancePlcTime/resetPlcTime`.
- **Gateway sync & proxy**: `S7ProxySession(src, hub)` with
  `addMapping/start/stop/running`; `GatewaySync(runtime)` with
  `subscribeDb/unsubscribeDb/publishOnDirty/connect/disconnect/connected/
  lastError`.
- **REPL conveniences**: bare expressions auto-print (`toJson` for
  DataBlock/TagTable/S7PathBatch, `toString` for HexTable/DTL/FieldProxy,
  `info()` for diagnostics, `print()` otherwise); primitives print as-is.
- **Init script** (`./angelscript.as`, loaded at startup): define shared
  globals/helpers (e.g. a connected `S7Client@ plc` plus `void cycle()`)
  visible in every REPL line and `runScript()`.

## Dependencies

Links `sgrn::core`, `sgrn_s7shell` (shell, bindings) and
cxxopts; readline wiring comes from `s7shell.cmake` in this directory.
