# sgrn/apps — entry points

One directory per executable: `main.cpp` + `CMakeLists.txt` (+ `README.md`).
Library code lives in `sgrn/lib/`; apps only wire mains to libs. Or run
`scripts/scaffold.py app <name>` to generate this layout.

## Adding a new app

1. `sgrn/apps/<name>/main.cpp` — the entry point. Copy the closest
   existing app; keep the `runMain`/`main_cb` exit-code convention.
2. `sgrn/apps/<name>/CMakeLists.txt`:
   - `sgrn_add_component_executable(TARGET <name> SOURCES main.cpp ...)`
     (target name = binary name; user-facing, don't prefix).
   - `PRIVATE_LIBS` for everything it links; `PCH` only if it needs
     `sgrn_pch_s7`/`sgrn_pch_net` (most apps don't).
   - `sgrn_target(<name>)` then
     `sgrn_install_component_executable(TARGET <name> COMPONENT <comp>)`.
   - `target_include_directories` with `${CMAKE_SOURCE_DIR}`-absolute
     paths only — never `${CMAKE_CURRENT_SOURCE_DIR}/include` (there is
     no include tree here).
3. Gate the subdir in this file's list below on the same `SGRN_BUILD_*`
   option as the libraries it needs (`WIN_LIBS_LIST` and Threads are
   already provided here).
4. Packaging: add the target to the right `sgrn_package_runtime_dependencies`
   bundle at the bottom of this file (targets must exist first — that is
   why packaging lives here, not in the lib files).
5. If integration-tested, add the binary path to `tests/ts/src/GatewayProcess.ts`
   or `tests/run_tests.py` (binaries land in
   `.build/<preset>/sgrn/apps/<name>/<name>`).
6. Write `sgrn/apps/<name>/README.md`: role, usage (quote `--help`),
   config, dependencies.

## Conventions

- One `main.cpp` per app; anything reusable belongs in `sgrn/lib/`.
- Generated sources (asset headers) stay with the producing library and are
  shared via `CACHE INTERNAL` dir variables (see `sgrn_datastore` precedent).
