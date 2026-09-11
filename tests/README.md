# tests — single interface: `python3 tests/test.py`

Every suite runs through `tests/test.py`: bare run opens the interactive
menu (grouped by area, each entry showing what it verifies — in-process
code behavior vs live network API — plus readiness), or drive it
non-interactively:

```bash
python3 tests/test.py --list            # all entries + readiness
python3 tests/test.py cpp               # one entry (name, number, or range)
python3 tests/test.py 1,3-5             # selection, menu numbering
python3 tests/test.py all [sim]         # everything runnable
python3 tests/test.py <name> --message  # describe one entry
```

Entries whose prerequisites are missing (no `bun`, no build dir/binaries)
are listed as UNAVAILABLE and SKIPPED, never failed. A run ends with a
result table grouped by offline / integration / suite, with per-entry
durations; exit code is non-zero on any failure or fixture error.

## Layout

- **`test.py`** — the runner. Spawns fixture processes (gateway + sim),
  runs one suite, `all`, or a category; exits non-zero on any failure.
- **Python suites, by area** (run directly too: `python3 tests/<area>/<name>.py`):
  - `gateway/` — `rest_api.py` (offline), `websocket.py`, `opcua_discovery.py`,
    `modbus.py` (integration, need a live gateway via the fixture).
  - `scl/` — `dtypes_endianness.py`, `schema_registry_validation.py` (offline).
  - `advanced/` — `array_support.py` (offline), `binary_live_snapshot.py`
    (integration, heavy).
- **`ts/`** — TypeScript E2E suite (`bun test`): gateway API, policy,
  registry, websocket, southbound protocols, s7shell, dashboard, data.
  Spawns its own isolated gateway/s7shell binaries (see `src/GatewayProcess.ts`
  for the expected `.build/.../sgrn/apps/*` paths).
- **C++ suites** — live next to their components (CMake convention, run via
  `ctest` or the `cpp` entry here, never moved into `tests/`):
  `sgrn/lib/{utils,gateway,s7shell}/tests/`, wired with `add_test`.
- **TS granularity** — one file per scenario (`ts-southbound-*`,
  `ts-dashboard`, …). The former `southbound-protocols.test.ts` monolith
  was split byte-exactly (all 31 cases preserved); shared fixture lives in
  each file's `beforeAll`/`afterAll`.

## Known-blocked (pre-existing, unrelated to the split)

Gateway-backed TS entries currently fail in this tree for two independent,
pre-existing reasons: (1) the default `gas_processing` simulation schema
does not parse (`sclc` rejects it; untouched by any refactor — nuclear
compiles fine); (2) even with a working sim, sim-driven data takes ~20s
while bun's default `beforeAll` hook budget is 5s. Verified: gateway serves
in ~2s manually, and a nuclear-backed probe passes end-to-end with an
explicit hook timeout.

## Suites reachable only through `test.py`

- `cpp` — full CTest run against `.build/linux-static-release`.
- `ts-suite` — full `bun test` run.
- `all` — every registry entry in order (integration entries boot the
  fixture with the `gas_processing` simulation unless given one).

## Manual scripts (not in the runner)

- `tests/gateway/enum-tests.py` — enum edge-case harness; needs a live
  gateway on non-default ports (`--opcua/--http` args), so it stays manual.
- `sgrn/lib/gateway/simulations/types/test_types.py` — full-coverage type
  walk for the `types` sim; same reason (custom ports + sim).
- Orphaned C++ sources with no CMake target (not built, not run):
  `sgrn/lib/scl/tests/scl_scalar_alias_tests.cpp`,
  `sgrn/lib/codecs/s7codec/example_test.cpp` (see its README one-liner),
  `sgrn/lib/gateway/tests/opcua_projection_test.cpp`. Wire or remove on
  purpose — do not let the list grow silently.
