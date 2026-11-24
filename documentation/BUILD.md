# SGRN Build, Dependency & Deployment Guide

This document explains SGRN's **xmake** build system, dependency management, cross-compilation targets, and how to deploy and bootstrap `sgrn_datastore` on a target Linux host.

> **Note:** SGRN has migrated from CMake + Micromamba to **xmake**. No Micromamba/Conda environment or `aws-sdk-cpp` installation is required.

---

## Directory Layout

| Path                  | Purpose                                                                               |
| :-------------------- | :------------------------------------------------------------------------------------ |
| `xmake.lua`           | Root build definition (toolchains, C++23 standard, rules, targets)                    |
| `deps-versions.lua`   | Centralized dependency version pins (`SGRN_DEPS`)                                     |
| `xmake/options.lua`   | Build options (`xmake f --name=value`)                                                |
| `xmake/packages.lua`  | Third-party dependency declarations (`xmake-repo` + custom recipes)                   |
| `xmake/packages/`     | Custom xmake package recipes (`drogon`, `open62541`, `snap7`, `libpq`, etc.)          |
| `xmake/rules.lua`     | Compiler policy, hardening flags, sanitizers, and PCH                                 |
| `xmake/tasks.lua`     | Custom tasks (`stage`, `generate-views`, `crud-generate`)                             |
| `xmake/modules/sgrn/` | Lua modules for VFS asset compression, web bundling, and ORM/CRUD codegen             |
| `sgrn/`               | SGRN libraries, apps, CRUD manifests, and web dashboards                              |
| `.build/`             | xmake build artifacts, compiled binaries (`.build/<plat>/<arch>/<mode>/`), and generated headers |
| `.prefix/`            | Staged public headers (`.prefix/include/sgrn`) and cached LibTorch                    |

---

## 1. Prerequisites

### Build Prerequisites (Development / CI Host)

- **xmake** `>= 2.8.5` (on Ubuntu: `sudo add-apt-repository -y ppa:xmake-io/xmake && sudo apt install -y xmake`; on Debian Trixie+: `sudo apt install -y xmake`)
- **C++23 compiler**: `gcc` / `g++` (recommended on Linux; locked by `xmake.lua`) or `clang`
- **Python 3** (for ORM/CRUD codegen and asset generation)
- **[Bun](https://bun.sh/)** (builds the React/Svelte web dashboards; pass `--web=n` to reuse existing `dist`/`build` outputs)
- **System dev packages (native Linux — used as prebuilt C libraries by `xmake/packages.lua`)**:
  ```bash
  # Debian / Ubuntu
  sudo apt install build-essential cmake ninja-build pkg-config \
    libssl-dev zlib1g-dev libzstd-dev libsqlite3-dev libc-ares-dev uuid-dev \
    libreadline-dev zstd python3
  ```
- **Cross-compilation toolchains (optional)**:
  - Windows x64: `x86_64-w64-mingw32-gcc` / `x86_64-w64-mingw32-g++` (`mingw-w64`)
  - Linux ARM64: `aarch64-linux-gnu-gcc` / `aarch64-linux-gnu-g++` (`g++-aarch64-linux-gnu`)

### Runtime Prerequisites (Target Linux Deployment Host)

On a target machine running `sgrn_datastore`:

- **PostgreSQL** (`sudo apt install postgresql` / `dnf install postgresql-server`) — managed by system `postgresql.service`
- **NGINX** (`sudo apt install nginx` / `dnf install nginx`) — started by `SGRN-nginx.service` with SGRN's own `nginx.conf`
- **Garage v2.x** — single binary placed on `PATH` (e.g., `/usr/local/bin/garage` or alongside `sgrn_datastore` in `SGRN_BIN_DIR`)

---

## 2. Building SGRN

All third-party dependencies are resolved and cached automatically by `xmake` on first configure (`xmake/packages.lua`).

### Native Linux (Static Release — Default)

```bash
xmake f -p linux -m release -y
xmake
```

Outputs:

- Binaries (apps & tests): `.build/linux/x86_64/release/` (`sgrn_datastore`, `gateway`, `s7shell`, `mbproxy`, `sgrn_replay`, `sgrn_dataset`, `sclc`, `sgrn_wal_dump`, `datastore_shell`, etc.)
- Generated assets & ORM headers: `.build/gen/`

### Native Linux (Debug with Shared Libraries)

```bash
xmake f -p linux -m debug -k shared -y
xmake
```

### Cross-Compilation

> `datastore` and `torch` are automatically disabled on `mingw`, `windows`, and `cross` targets (`xmake/options.lua`).

```bash
# Windows x86_64 (MinGW, static runtime) -> .build/mingw/x86_64/release/
xmake f -p mingw -a x86_64 -m release -y
xmake -j"$(nproc)"

# Linux ARM64 -> .build/cross/arm64/release/
xmake f -p cross -a arm64 --cross=aarch64-linux-gnu- -m release -y
xmake -j"$(nproc)"
```

### Running Tests

Tests are enabled and compiled by default (`--tests=y`):

```bash
xmake -j"$(nproc)"
xmake test -v -j"$(nproc)"
```

---

## 3. Build Options & Custom Tasks

Configure options with `xmake f --<option>=<value>`:

| Option         | Default            | Description                                                                                              |
| :------------- | :----------------- | :------------------------------------------------------------------------------------------------------- |
| `datastore`    | `y` (native Linux) | Build the datastore backend (Drogon / PostgreSQL)                                                        |
| `torch`        | `y` (non-Windows)  | Build LibTorch ML targets (`--torch=n` to disable)                                                       |
| `torch_dir`    | `""`               | Custom LibTorch root (else `$LIBTORCH`, `$TORCH_DIR`, conda, or auto-downloaded to `.prefix/libtorch-*`) |
| `opc`          | `y`                | Enable OPC UA support in `s7shell`                                                                       |
| `tests`        | `y`                | Build test executables (compiled by default; run via `xmake test`)                                      |
| `pch`          | `n`                | Enable precompiled headers (`xmake/pch/`)                                                                |
| `native`       | `y` (native)       | Pass `-march=native` in release builds                                                                   |
| `web`          | `y`                | Rebuild web dashboards with Bun (`n` = reuse existing build output)                                      |
| `sanitize`     | `""`               | Sanitizers: `asan`, `ubsan`, `tsan`, or `asan,ubsan`                                                     |
| `auto_codegen` | `y`                | Auto-regenerate ORM/CRUD views when SQL or manifests change (`xmake/modules/sgrn/codegen.lua`)           |

Custom tasks:

```bash
xmake stage            # Copy public headers into .prefix/include/sgrn
xmake generate-views   # Regenerate Drogon ORM models and CRUD views
xmake crud-generate    # Regenerate CRUD handlers from YAML manifests
```

---

## 4. Dependency Management

Dependencies are pinned in `deps-versions.lua` and declared in `xmake/packages.lua`:

- C++ and version-pinned packages (`fmt`, `jsoncpp`, `cxxopts`, `unordered_dense`, `glaze`, `crow`, `asio`, `cpp-httplib`, `ixwebsocket`) come from `xmake-repo` with `{shared = false}`.
- Standard C system libraries (`openssl`, `zlib`, `zstd`, `sqlite3`, `c-ares`, `libuuid`) use prebuilt system `-dev` packages on native Linux (falling back to `xmake-repo` if absent), and automatically switch to target-arch static source builds (`system = false`) when cross-compiling.
- Custom/niche packages (`snap7`, `angelscript`, `opener`, `sqlite_modern_cpp`, `open62541`, `drogon`, `libmodbus`, `xml_h`, `reproc`, `libpq`, `readline`, `sgrn_torch`) live in `xmake/packages/*.lua`.
- S3 operations use the built-in `NativeS3Client` (Drogon `HttpClient` + OpenSSL SigV4) — `aws-sdk-cpp` is no longer used.

---

## 5. Deploying & Bootstrapping `sgrn_datastore`

`sgrn_datastore` is self-contained: it embeds the React dashboard, flattened PostgreSQL schema, NGINX configs, and systemd unit templates. You can copy the single `sgrn_datastore` binary to any Linux machine with `postgresql`, `nginx`, and `garage` installed.

### Quick Start on a Target Machine

```bash
# 1. Install system runtime dependencies
sudo apt install postgresql nginx    # or dnf install postgresql-server nginx
# Ensure `garage` (v2.x) is in /usr/local/bin/ (or the same directory as sgrn_datastore)

# 2. Copy the binary
sudo cp .dist/linux-static/bin/sgrn_datastore /usr/local/bin/

# 3. Initialize secrets, configs, self-signed TLS certs, and PostgreSQL schema
#    (Add --non-interactive to auto-generate all secrets without prompts)
sudo sgrn_datastore --init

# 4. Deploy and start systemd units
sudo sgrn_datastore --config-systemd

# 5. Verify installation & listening ports
sgrn_datastore --verify-install --allow-insecure
```

### How Bootstrapping Works

- **Operation directory**: Stored at `~/.local/share/sgrn` of the invoking user (`SUDO_USER` is respected when invoked via `sudo`, and files are `chown`ed back to that user).
- **PostgreSQL**: Uses the system `postgresql.service` on `127.0.0.1:5432`. When `--init` or `--init-db` runs via `sudo` without `POSTGRES_SUPERUSER_PASSWORD`, it automatically bootstraps via Unix socket `peer` auth as the system `postgres` user (`su -s /bin/sh postgres`), requiring no manual `createuser` or `pg_hba.conf` edits.
- **NGINX**: `SGRN-nginx.service` runs `/usr/bin/env nginx` using SGRN's staged `${SGRN_DATA_DIR}/nginx/nginx.conf`, with temp buffers (`client_body_temp_path`, `proxy_temp_path`, etc.) directed inside `${SGRN_DATA_DIR}/var/tmp/nginx/` so non-root NGINX works under systemd `ProtectSystem=strict`.
- **Systemd units deployed**:
  - `SGRN-datastore.service` (`:5000`, requires `postgresql.service`, `SGRN-garage.service`, `SGRN-nginx.service`)
  - `SGRN-garage.service` (`:3900` S3, `:3903` admin)
  - `SGRN-nginx.service` (`:80` redirect, `:443` TLS reverse proxy)
  - `sgrn.service` (umbrella unit)

### Useful Maintenance Flags

```bash
sgrn_datastore --generate-config        # Re-render staged configs & systemd templates without touching .env secrets
sudo sgrn_datastore --init-db --force   # Drop & recreate database from embedded schema
sgrn_datastore --rotate POSTGRES_PASSWORD --rotate JWT_SECRET
sgrn_datastore --backup-db              # pg_dump backup into ~/.local/share/sgrn/backup/
sgrn_datastore --backup-garage          # S3 object backup with SHA-512 verification
```
