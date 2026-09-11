# sgrn/lib — libraries

One directory per component: `include/` + `src/` (+ `tests/`), wired in
`lib/CMakeLists.txt`. Or run `scripts/scaffold.py lib <name>`.

## Naming rule (enforced by convention, checked in review)

- Target `sgrn_<dir>`, alias `sgrn::<dir>` (e.g. `sgrn_scl` /
  `sgrn::scl`, `sgrn_common` / `sgrn::common`).
- Known exceptions, do not "fix":
  - `sgrn_datastore_lib` (+ `sgrn_datastore_orm`): the `sgrn_datastore`
    executable owns the clean name.
  - `sgrn_gateway_*` family + shared `sgrn_gateway`: sub-library grouping
    under one component.
- Executable targets keep user-facing bare names (e.g. `gateway`,
  `s7shell`, `sclc`) and live in `sgrn/apps/`, never here.

## Adding a new library

1. `sgrn/lib/<name>/` with `include/sgrn/<name>/`, `src/`, `CMakeLists.txt`:
   - Compiled: `sgrn_add_component_library(TARGET sgrn_<name> ALIAS
     sgrn::<name> TYPE STATIC|SHARED, SOURCES, INCLUDE_DIR
     ${CMAKE_CURRENT_SOURCE_DIR}/include, PUBLIC/PRIVATE_LIBS)` +
     `sgrn_target(...)`, PIC + C++23 props like the neighbors.
   - Header-only: copy `sgrn/lib/core/CMakeLists.txt` (INTERFACE target,
     `BUILD/INSTALL_INTERFACE` includes, `sgrn_install_component_library`).
   - Use `${CMAKE_CURRENT_SOURCE_DIR}` inside the component file;
     `${CMAKE_SOURCE_DIR}/sgrn/lib/...` only to reach *other* components.
2. One gate line in `lib/CMakeLists.txt` on the matching `SGRN_BUILD_*`
   option (declare the option in `sgrn/CMakeLists.txt` if new).
3. Staging (`cmake/staging_sgrn.cmake`, otherwise the lib silently never
   reaches `.prefix/`): add to the stage loop + headers line, plus an
   IMPORTED approximation block mirroring the real link interface.
4. If the lib introduces error types used across directories, extend
   `scripts/lints/check_boundaries.sh` (see its `BOUNDARIES` table).
5. Tests: `SGRN_BUILD_TESTS`-gated `sgrn_add_component_executable` +
   `add_test` next to the component (see `sgrn/lib/utils` precedent).

## Conventions

- Public headers only under `include/`; namespace `sgrn::<name>`.
- No component reaches into another's `src/` — depends on targets, not files.
- New protocol adapters depend on `sgrn::common` ports, never on
  `sgrn_gateway_twin`/`sgrn_gateway_security` directly.
