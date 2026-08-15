# SGRN Build System Guide

This document explains the development environment, build workflow, dependency management, and how to extend the system with new third-party libraries.

---

## Directory Layout

| Directory    | Purpose                                                                 |
|:-------------|:------------------------------------------------------------------------|
| `cmake/`     | All CMake logic: global config, dep wiring, platform resolvers          |
| `cmake/deps/`| Third-party dep declarations (CPM), per-dep cmake files, staging        |
| `deps/`      | Deps-only sub-project CMakeLists.txt (Phase 1 entry point)              |
| `sgrn/`      | SGRN component source trees                                             |
| `.build/`    | CMake build directories — one per preset (gitignored)                   |
| `.prefix/`   | Staged sysroot: compiled deps + headers (gitignored, populated Phase 1) |
| `.dist/`     | Final distribution bundles (gitignored, populated by `--target install`)|
| `~/.cache/CPM/` | Machine-level CPM source cache — survives build dir wipes           |

> **Note:** There are no git submodules. All third-party source code is downloaded
> on first configure via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) and
> cached persistently in `~/.cache/CPM/`.

---

## Two-Phase Build Overview

SGRN uses a **two-phase CMake build**:

```
Phase 1 — deps build      cmake --preset linux-deps-static
                          cmake --build --preset linux-deps-static --target install
                               │  (downloads sources, compiles, stages)
                               ▼
                          .prefix/
                            include/               ← all headers (platform-neutral)
                            linux-static/lib/      ← compiled .a archives
                            linux-static/lib/cmake/extern/extern-config.cmake

Phase 2 — main build      cmake --preset linux-static
                          cmake --build --preset linux-static
                               │  find_package(extern) → extern-config.cmake
                               ▼
                          .dist/linux-static/      ← executables + runtime deps
```

Phase 1 only needs to run **once per platform**. After that, Phase 2 builds are
fully incremental.

---

## 1. Environment Setup

SGRN uses a **hybrid toolchain** — compilers from the host conda env, libraries
from target-specific environments.

### Host Environment (required for all targets)

```bash
micromamba create -f SGRN.yml
micromamba activate SGRN
```

### Target Sysroot Environments (cross-compilation only)

> **DO NOT activate these.** They are used only as library prefix directories.

```bash
micromamba create -f micromamba/SGRN-WIN64.yml  --platform win-64
micromamba create -f micromamba/SGRN-ARM64.yml  --platform linux-aarch64
```

---

## 2. First-Time Setup: Source Cache

CPM downloads all third-party sources to `~/.cache/CPM/` on first configure.
This is machine-level — switching presets or wiping `.build/` does **not**
trigger re-downloads.

```bash
# Optional: override the cache location (already set in CMakePresets.json)
export CPM_SOURCE_CACHE=~/.cache/CPM

# First configure downloads CPM.cmake itself (~40 KB) then all deps
cmake --preset linux-deps-static
```

After the first configure completes:
- All dep sources are in `~/.cache/CPM/`
- Running `cmake --preset linux-arm64-deps-static` or `cmake --preset win64-deps-static`
  reuses those sources — **zero re-downloads**

---

## 3. Build Workflow

### Phase 1: Build & Stage Dependencies

```bash
# Linux x64 (static)
cmake --preset linux-deps-static
cmake --build --preset linux-deps-static --target install

# Linux ARM64 (cross-compile)
cmake --preset linux-arm64-deps-static
cmake --build --preset linux-arm64-deps-static --target install

# Windows x64 (cross-compile)
cmake --preset win64-deps-static
cmake --build --preset win64-deps-static --target install
```

Or use the workflow shorthand (configure + build + install in one):

```bash
cmake --workflow --preset linux-deps-static
```

### Phase 2: Build SGRN

```bash
# Linux x64
cmake --preset linux-static && cmake --build --preset linux-static

# Linux ARM64
cmake --preset linux-arm64-static && cmake --build --preset linux-arm64-static

# Windows x64
cmake --preset win64-static && cmake --build --preset win64-static
```

Phase 2 auto-detects the staged prefix by checking for:
`.prefix/<platform>/lib/cmake/extern/extern-config.cmake`

---

## 4. Sysroot Layout (`.prefix/`)

After Phase 1, `.prefix/` looks like:

```
.prefix/
├── include/                       ← ALL headers (shared, one copy per dep)
│   ├── fmt/
│   ├── json/                      ← jsoncpp
│   ├── open62541/
│   ├── snap7cpp/
│   ├── angelscript/
│   ├── ixwebsocket/
│   ├── drogon/  trantor/
│   ├── modbus/
│   ├── opener/
│   ├── rapidjson/
│   ├── httplib/
│   ├── s7codec/
│   └── sqlite_modern_cpp.h
│
└── linux-static/                  ← platform-specific
    └── lib/
        ├── libfmt.a
        ├── libjsoncpp.a
        ├── libopen62541.a
        ├── libsnap7cpp.a
        ├── libangelscript.a
        ├── libangelscript_addons.a
        ├── libixwebsocket.a
        ├── libdrogon.a
        ├── libtrantor.a
        ├── libmodbus.a
        ├── libopener.a
        └── cmake/extern/
            └── extern-config.cmake
```

---

## 5. Dependency Management

All third-party dependencies are managed by **CPM.cmake** via
`cmake/deps/FetchDeps.cmake`. There are **no git submodules**.

### How sources are fetched

| Strategy      | Used for                          | Speed        |
|:--------------|:----------------------------------|:-------------|
| URL tarball   | Deps with GitHub release archives | Fastest      |
| Shallow clone | Deps with no release archives     | Fast         |

CPM caches sources in `~/.cache/CPM/` (set by `CPM_SOURCE_CACHE` in
`CMakePresets.json`). This cache is:
- **Machine-level**: shared across all presets and projects
- **Persistent**: survives `rm -rf .build/`
- **Deduplicated**: the same version of a dep is never downloaded twice

### Current dependencies

| Library          | Version    | Fetch method  |
|:-----------------|:-----------|:--------------|
| fmt              | 12.1.0     | URL tarball   |
| jsoncpp          | 1.9.8      | URL tarball   |
| open62541        | v1.4.15    | URL tarball   |
| ixwebsocket      | v12.0.1    | URL tarball   |
| libmodbus        | v3.1.12    | URL tarball   |
| cpp-httplib      | v0.53.0    | URL tarball   |
| sqlite_modern_cpp| v3.2       | URL tarball   |
| snap7            | main       | Shallow clone |
| angelscript      | SHA pinned | Shallow clone |
| opener           | master     | Shallow clone |
| drogon           | v1.9.13    | Shallow clone |
| rapidjson        | master     | Shallow clone |
| xml_h            | SHA pinned | Shallow clone |

---

## 6. Adding a New Third-Party Dependency

### Step 1 — Register in `cmake/deps/FetchDeps.cmake`

Add the download metadata. Prefer URL tarballs for deps with GitHub releases:

```cmake
# Option A: URL tarball (preferred — fastest)
set(SGRN_DEP_mylib_URL "https://github.com/org/mylib/archive/refs/tags/v1.2.3.tar.gz")

# Option B: Shallow git clone (for deps without release archives)
set(SGRN_DEP_mylib_REPO "https://github.com/org/mylib.git")
set(SGRN_DEP_mylib_TAG  "v1.2.3")
```

### Step 2 — Create `cmake/deps/mylib.cmake`

This file is responsible for building the library:

```cmake
# cmake/deps/mylib.cmake
# Exposes: extern::mylib

if(TARGET extern::mylib)
    return()
endif()

sgrn_fetch_source(mylib)   # ← downloads + sets mylib_SOURCE_DIR

# Set any CMake options BEFORE add_subdirectory
set(MYLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${mylib_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/mylib"
    EXCLUDE_FROM_ALL
)

if(TARGET mylib)
    add_library(extern::mylib ALIAS mylib)
    sgrn_add_to_install(mylib)
endif()
```

For **header-only** deps, skip `add_subdirectory` and just create an INTERFACE target:

```cmake
sgrn_fetch_source(mylib)
if(NOT TARGET extern::mylib)
    add_library(mylib INTERFACE)
    target_include_directories(mylib SYSTEM INTERFACE "${mylib_SOURCE_DIR}/include")
    add_library(extern::mylib ALIAS mylib)
endif()
```

### Step 3 — Include in `deps/CMakeLists.txt`

Add the include alongside the other deps:

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/deps/mylib.cmake)
```

### Step 4 — Add to `cmake/deps/staging.cmake`

So the library and its headers are installed into `.prefix/`:

```cmake
# Section 1 — library install (if compiled, not header-only)
# The sgrn_add_to_install() call in step 2 handles this automatically.

# Section 2 — header install
install(DIRECTORY "${mylib_SOURCE_DIR}/include/mylib"
    DESTINATION "${_INC}"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# Section 3 — add an imported target to extern-config.cmake
# Append to EXTERN_CONFIG_CONTENT near the other target blocks:
string(APPEND EXTERN_CONFIG_CONTENT "
add_library(extern::mylib STATIC IMPORTED GLOBAL)
set_target_properties(extern::mylib PROPERTIES
    IMPORTED_LOCATION \"\${_lib}/libmylib.a\"
    INTERFACE_INCLUDE_DIRECTORIES \"\${_inc}\"
)
")
```

### Step 5 — Wire into the root `CMakeLists.txt`

```cmake
# Declare the sgrn:: alias
sgrn_alias(sgrn::mylib)

# Link the source-build target to the alias
sgrn_link_dependency(sgrn::mylib mylib extern::mylib)
```

### Step 6 — Add fallback for non-installed-deps mode

If the library is consumed in normal (non-deps-only) builds, add it to
`cmake/deps/header_only.cmake` (for header-only) or create a small
`cmake/deps/mylib_consume.cmake` that uses `find_package` as fallback.

---

## 7. Key CMake Variables

| Variable                  | Set in                    | Meaning                                           |
|:--------------------------|:--------------------------|:--------------------------------------------------|
| `SGRN_SHARED_PREFIX`      | `cmake/global.cmake`      | `.prefix/` — architecture-neutral header root     |
| `SGRN_LOCAL_PREFIX`       | `cmake/global.cmake`      | `.prefix/<platform>` — platform-specific libs     |
| `SGRN_CONDA_PREFIX`       | `cmake/global.cmake`      | Active conda env root                             |
| `SGRN_USE_INSTALLED_DEPS` | `CMakeLists.txt`          | Auto-ON when `extern-config.cmake` exists         |
| `SGRN_DEPS_ONLY`          | preset (`base-deps`)      | ON in `*-deps*` presets — skips SGRN components  |
| `SGRN_BUILD_STATIC`       | preset                    | ON for static builds                              |
| `CPM_SOURCE_CACHE`        | `CMakePresets.json`       | `~/.cache/CPM` — persistent dep source cache      |
| `FETCHCONTENT_BASE_DIR`   | `CMakePresets.json`       | `.build/_deps` — build-local fallback cache       |

---

## 8. File Map

### `cmake/deps/` — Dependency layer

| File                    | Role                                                                 |
|:------------------------|:---------------------------------------------------------------------|
| `FetchDeps.cmake`       | CPM bootstrap + all dep download metadata + `sgrn_fetch_source()`   |
| `<dep>.cmake`           | Per-dep: populate source, set options, add_subdirectory, alias       |
| `jsoncpp_consume.cmake` | Consumer-side jsoncpp resolver (installed/source/system fallback)    |
| `staging.cmake`         | Phase 1 install pipeline → `.prefix/` + generates `extern-config.cmake` |
| `hardening.cmake`       | Compiler hardening flags for the deps build                          |
| `linux.cmake`           | Linux-native dep resolver (Phase 2)                                  |
| `arm64.cmake`           | ARM64 cross-compile dep resolver (Phase 2)                           |
| `win64.cmake`           | Win64 cross-compile dep resolver (Phase 2)                           |
| `fmt.cmake`             | `sgrn::fmt` resolver (deps-only produce + normal consume)            |
| `header_only.cmake`     | INTERFACE targets: rapidjson, httplib, xml_h, sqlite_modern_cpp      |
| `s7codec.cmake`         | s7codec header-only target                                           |

### `cmake/` — Main build logic

| File                  | Role                                                                     |
|:----------------------|:-------------------------------------------------------------------------|
| `global.cmake`        | Source of truth — prefix paths, `sgrn_alias()`, `sgrn_link_dependency()` |
| `macros.cmake`        | Low-level CMake utilities                                                 |
| `targets.cmake`       | Component library/executable helpers                                     |
| `deps.cmake`          | Orchestrator — includes platform resolver + header_only                  |
| `pch.cmake`           | Tiered PCH system                                                        |
| `packaging.cmake`     | Runtime `.so` harvester for distribution bundles                         |
| `staging_sgrn.cmake`  | SGRN's own install pipeline + `sgrn-config.cmake` generation             |

---

## 9. Troubleshooting

| Symptom | Fix |
|:--------|:----|
| `CPM.cmake` download fails | Place `CPM_0.42.0.cmake` manually in `~/.cache/CPM/cpm/` |
| Dep source not found | Delete `~/.cache/CPM/<dep>/` and re-configure to force re-download |
| `find_package(extern)` fails | Run the appropriate `*-deps*` preset first to populate `.prefix/` |
| Wrong headers after dep update | `rm -rf .prefix/ .build/` and rebuild from Phase 1 |
| ARM64: missing `string_view` | Ensure `SGRN-ARM64` conda env is up-to-date (`micromamba update`) |
| Windows: missing DLLs in `.dist/` | Verify dep is in `SGRN-WIN64` conda env and `SGRN_WIN64_PREFIX` is set |
| clangd / LSP shows no symbols or "no compilation database found" | The root `compile_commands.json` symlink is missing. Run `./scripts/link_ccab.sh` (see [Editor / Language Server](#10-editor--language-server-clangd) below) |

---

## 10. Editor / Language Server (clangd)

### How clangd finds the compilation database

clangd reads `compile_commands.json` from the project root. That root file is
**not** committed to git — it is a symlink (managed by `.gitignore`) that points
into the build tree:

```
compile_commands.json → .build/<preset>/compile_commands.json
```

CMake generates the real file inside each build directory because the
`base-common` preset sets `CMAKE_EXPORT_COMPILE_COMMANDS=ON`.

### Linking the database

After any successful `cmake --preset` configure, run:

```bash
./scripts/link_ccab.sh              # defaults to linux-static-release
./scripts/link_ccab.sh linux        # switch to the Debug linux preset
```

The script validates that the target build's `compile_commands.json` exists
before creating the symlink, and will error out with a hint to run `cmake --preset`
first if it doesn't.

After re-linking, restart the language server to pick up the new database:

```vim
" Neovim (nvim-lspconfig)
:LspRestart
```

### If clangd still shows no symbols

1. **Verify the symlink resolves:** `readlink -f compile_commands.json`
2. **Verify it's valid JSON:** `python3 -c "import json; print(len(json.load(open('compile_commands.json'))))"`
3. **Check the build directory actually ran CMake configure** — if
   `.build/linux-static-release/compile_commands.json` is missing, re-run
   `cmake --preset linux-static-release` and then `./scripts/link_ccab.sh` again.
4. **Check clangd logs** (`:checkhealth` in nvim or `clangd --log=verbose` from
   the terminal) for "no compilation database found" messages pointing to a
   stale or broken symlink.
