# Writing and launching SGRN tests

All suites run through one orchestrator: `tests/run_tests.py`.
(`tests/test.py` is a deprecated shim that forwards to it.)

```bash
python3 tests/run_tests.py --list            # every entry + readiness
python3 tests/run_tests.py <name> [--message]# run one entry / show its description
python3 tests/run_tests.py --module gateway  # run a whole module
python3 tests/run_tests.py all [sim]         # everything runnable
```

Entries are grouped by the **module** they verify (`gateway`, `scl`,
`datastore`, `s7shell`, `utils`) and tagged with a **kind**
(`python` | `ts` | `cpp`) and a **mode** (`offline` | `integration` |
`suite`). The interactive menu (bare `run_tests.py`) offers the same
selection by number/range. Exit code is non-zero on any failure or
fixture error. Entries whose prerequisites are missing (no `bun`, no
build dir/binaries) are UNAVAILABLE/`SKIP`, never failed; entries marked
`needs_setup` are skipped by `all` but run when selected explicitly.

## Launching: what happens per kind

- **python/offline** — executed directly: `python3 tests/<module>/<name>.py [args]`.
- **python/integration** — the orchestrator spawns an isolated gateway
  fixture first (config + state under `/tmp/gateway-state-<sim>-<ts>`,
  fixed ports **HTTP/WS 8080, OPC UA 4840, S7 8102**, schema + policy from
  `sgrn/lib/gateway/simulations/<sim>`, default sim `gas_processing`),
  then runs the script; the fixture is torn down afterwards. An optional
  positional `sim` argument (or the `Simulation:` prompt) overrides the
  simulation; per-entry `args`/`env` from the registry are applied.
- **ts** — `bun test <file>` (or the whole dir for `ts-suite`) in
  `tests/ts/`; each file spawns its own gateway via `src/GatewayProcess.ts`
  (expects release binaries under `.build/linux-static-release/...`).
- **cpp** — `ctest -R ^<name>$` in the auto-detected build dir (release
  preferred, debug fallback); `cpp` runs the whole `ctest`.

## Writing a Python test (`tests/<module>/<name>.py`)

1. Put the file in the directory of the module it verifies
   (`tests/gateway/`, `tests/scl/` — create the dir if a new module
   needs one). Filename must be a valid module name (`snake_case.py`).
2. Start the file with a **docstring** — its first paragraph *is* the
   description shown in `--list` and `--message`. State what is verified
   and any requirements (ports, packages, simulations).
3. Make it directly executable: `#!/usr/bin/env python3`, parse args with
   `argparse` under `if __name__ == "__main__":`, exit `0` on success and
   non-zero on failure (plain `assert`s or a PASS/FAIL summary both work).
4. For integration tests, accept the fixture's layout instead of hardcoding
   your own: default to host `127.0.0.1`, HTTP `8080`, OPC UA `4840`, and
   expose `--db/--field`-style overrides for schema-dependent targets.
   Never write state into the repo tree (use `/tmp`); prefer env vars
   (`SGRN_LIVE_*` pattern) for optional behavior switches.
5. Register it in `PYTHON_TESTS` in `tests/run_tests.py`:
   ```python
   dict(name="gateway-myfeature", module="gateway", mode="integration",
        path="gateway/myfeature.py", simulation="gas_processing",
        args=["--http", "http://localhost:8080"],
        env={"SGRN_LIVE_EXTERNAL": "1"},
        needs_setup=False,  # True if no stock fixture satisfies it (skipped by `all`)
        assumptions="Requires ..."),
   ```
   `mode="offline"` entries run as-is; `mode="integration"` entries run
   inside the gateway fixture (fixed ports HTTP/WS 8080, OPC UA 4840, S7
   8102; state under `/tmp`). An entry-pinned `simulation` is a hard
   requirement and wins over the CLI positional sim, which is only the
   fallback for entries without one.

## Writing a TS test (`tests/ts/tests/<name>.test.ts`)

1. One file per surface; the `describe("...")` title *is* the description
   shown by the orchestrator, so make it say what behavior is covered
   (e.g. `"WebSocket Telemetry Tests"`).
2. Use `GatewayProcess` from `../src/GatewayProcess` to spawn an isolated
   gateway/s7shell in `beforeAll` and stop it in `afterAll`; never assume
   a shared gateway (files may run in any order).
3. Register it in `TS_TESTS` in `tests/run_tests.py` as
   `("<rel path>", "<module>", "<surface label>")` — module is the area
   under test (`gateway`, `s7shell`), surface is the one-line "verifies".

## Writing a C++ test (`sgrn/lib/<module>/tests/<name>.cpp`)

1. Start the file with a leading `//` comment block describing what the
   test proves — the orchestrator reads it as the entry description.
   Keep tests self-contained (`main()` + `CHECK`-style asserts, no gtest
   dependency, following the neighboring files).
2. Wire it with `add_executable` + `add_test` in the module's
   `CMakeLists.txt` (see `sgrn/lib/gateway/CMakeLists.txt` for the pattern).
   No orchestrator edit is needed: entries are discovered from the build
   via `ctest --show-only`, and the module is derived from the binary path
   (`sgrn/lib/<module>/...`).
3. Rebuild before running: the orchestrator never builds, it only checks
   the binary exists (missing binary → UNAVAILABLE/SKIP).

## Checklist before opening a PR

- `python3 tests/run_tests.py --list` shows the new entry under the right
  module with a one-line description (not the fallback).
- `python3 tests/run_tests.py <name> --message` reads correctly.
- The entry passes standalone **and** the module still passes:
  `python3 tests/run_tests.py --module <module>`.
- Integration tests also pass against a fresh fixture (no leftover
  `/tmp/gateway-state-*`, no hardcoded ports clashing with 8080/4840).
- C++ tests are registered with `add_test` so plain `ctest` sees them too.
