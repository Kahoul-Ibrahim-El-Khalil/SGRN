# tests — single interface: `python3 tests/run_tests.py`

Every suite runs through `tests/run_tests.py`, grouped by the **module** each
test verifies (`gateway`, `scl`, `datastore`, `s7shell`, `utils`) and by
**kind** (python | ts | cpp). See [WRITING_TESTS.md](WRITING_TESTS.md) for
the full guide: how to launch everything, and how to add a test of each
kind (where it lives, how its description is picked up, how to register it).

- **python** — suites under `tests/<module>/*.py`, run in-process (offline)
  or against a spawned gateway fixture (integration).
- **ts** — TypeScript E2E over the live network API (HTTP/WS/OPC-UA/S7);
  each file covers one surface end-to-end and spawns its own gateway.
- **cpp** — native unit tests living next to their components
  (`sgrn/lib/<module>/tests/`), discovered from the CMake build and run
  individually via `ctest -R` (plus the aggregate `cpp` entry for all).

Each entry's description is read from the test itself (Python docstring, TS
`describe()` title, leading C++ comment block) — `run_tests.py <name>
--message` shows it. The registry inside `run_tests.py` only holds runner
metadata (module, mode, args, assumptions).

```bash
python3 tests/run_tests.py --list            # all entries + readiness
python3 tests/run_tests.py cpp               # whole C++ suite
python3 tests/run_tests.py --module gateway  # one module
python3 tests/run_tests.py gateway-enums     # one entry (name, number, or range)
python3 tests/run_tests.py 1,3-5             # selection, menu numbering
python3 tests/run_tests.py all [sim]         # everything runnable
python3 tests/run_tests.py <name> --message  # describe one entry
```

Entries whose prerequisites are missing (no `bun`, no build dir/binaries)
are listed as UNAVAILABLE and SKIPPED, never failed. Entries marked
`needs_setup` (no stock fixture satisfies them, e.g. `gateway-arrays`)
are skipped by `all` but run when selected explicitly. A run ends with a
result table grouped by module, with per-entry durations; exit code is
non-zero on any failure or fixture error.

## Layout

- **`run_tests.py`** — the runner. Spawns fixture processes (gateway + sim),
  runs one entry, one module, `all`, or a category; exits non-zero on any
  failure. (`test.py` is a deprecated shim forwarding to it.)
- **Python suites, by module** (run directly too: `python3 tests/<module>/<name>.py`):
  - `gateway/` — `rest_api.py` (offline), `websocket.py`,
    `opcua_discovery.py`, `modbus.py`, `enum_tests.py`, `array_support.py`,
    `binary_live_snapshot.py` (integration, need a live gateway via the fixture).
  - `scl/` — `dtypes_endianness.py`, `schema_registry_validation.py` (offline).
- **`ts/`** — TypeScript E2E suite (`bun test`): gateway API, policy,
  registry, websocket, southbound protocols, s7shell, dashboard, data.
  Spawns its own isolated gateway/s7shell binaries (see `src/GatewayProcess.ts`
  for the expected `.build/.../sgrn/apps/*` paths).
- **C++ suites** — live next to their components (CMake convention, run via
  `ctest` or per-test `cpp-<name>` entries here, never moved into `tests/`):
  `sgrn/lib/{utils,scl,gateway,s7shell,datastore}/tests/`, wired with `add_test`.
