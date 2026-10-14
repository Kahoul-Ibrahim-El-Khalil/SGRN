# SGRN

**SGRN** is a high-performance, modular C++ and Python framework for industrial automation, edge digital twins, deterministic soft-PLC simulation, and schema-driven cyber-physical plant supervision.

SGRN eliminates the brittle, manual byte-offset mapping traditional in industrial telemetry integration by using **declarative SCL (Structured Control Language) schemas as the single source of truth**. It combines memory-safe, zero-allocation C++ edge runtimes with physics-informed, conformal machine learning for closed-loop plant monitoring.

> [!TIP]
> **Research Experiments:** For autonomous plant supervision, physics-constrained manifold anomaly detection, split-conformal uncertainty quantification, and the unified pipeline orchestrator, see [**`experiments/`**](./experiments/README.md) and [**`experiments/flare/`**](./experiments/flare/README.md).

---

## Key Capabilities & Objectives

- **Declarative Memory Coupling**: Parses Siemens S7 SCL schemas into dynamic memory maps and protocol ASTs, completely removing hardcoded byte offsets from edge middleware.
- **Protocol Convergence**: Ingests and normalizes multi-protocol industrial telemetry (Siemens S7, Modbus, OPC-UA, HTTP/JSON, WebSockets) into an in-memory shadow Digital Twin.
- **Deterministic Edge Execution**: Provides an AngelScript-driven soft-PLC (`s7shell`) capable of executing control logic, simulating complex thermodynamic processes, and enforcing safety interlocks with sub-millisecond determinism.
- **High-Throughput Write-Ahead Logging (WAL)**: Records atomic state changes into compressed Zstandard archives for loss-tolerant edge buffering and exact historical replay.
- **Trustworthy ML & Conformal Calibration**: Integrates real-time PyTorch autoencoders and split-conformal uncertainty quantification (`sgrn.ml`) to detect process drift with provable false-alarm guarantees.

---

## Codebase Architecture

The repository is organized into distinct, cohesive layers:

```
SGRN/
├── sgrn/                       # Core C++ libraries and runtime modules
│   ├── apps/                   # Executable entry points (gateway, s7shell, replay, dataset)
│   ├── lib/
│   │   ├── scl/                # Industrial SCL schema AST parser and memory layout generator
│   │   ├── gateway/            # Shadow memory digital twin, policy engine, OPC-UA/HTTP/WS hub
│   │   ├── s7shell/            # AngelScript soft-PLC runtime and process simulator
│   │   ├── datastore/          # Resilient time-series buffer and storage adapters
│   │   ├── scripting/          # AngelScript C++ bindings and sandboxing
│   │   ├── core/ & utils/      # Thread pools, zero-allocation allocators, IPC, and logging
│   │   └── sdk/                # C++ client SDK for external IT integrations
│   └── python/                 # Python package (sgrn)
│       ├── gateway.py          # Python client bindings for the gateway REST/WS APIs
│       └── sgrn/ml/            # Manifold ML, device abstraction, conformal UQ, and pipelines
├── experiments/                # Research benchmark suite & pipeline orchestrator
│   ├── flare/                  # Self-contained Gas Dehydration & Flare Blocked experiment
│   └── orchestrate.py          # Unified tqdm pipeline orchestrator (ingestion -> training -> evaluation)
├── demos/                      # Interactive gateway scenarios, soft-PLC simulations, and live twins
├── documentation/              # Technical specifications, build manuals, and architecture guides
└── actions/ & scripts/         # Automated database setup, web packaging, and code generation
```

---

## Core Utilities & Binaries

Building the project produces several core binaries:

| Utility | Description | Primary Role |
|:---|:---|:---|
| **`gateway`** | Real-time Telemetry Hub | Ingests PLC telemetry into a mutex-protected shadow digital twin; exposes OPC-UA, WebSockets, and HTTP APIs; enforces security policies; and serves embedded UI dashboards. |
| **`s7shell`** | Soft-PLC & Scripting Engine | Standalone execution environment running AngelScript against shadow memory; executes process physics and automates PDU-optimized read/write batching. |
| **`sgrn_replay`** | Deterministic WAL Replayer | Replays compressed historical `.jsonl.zst` or `.bin.zst` archives into running gateways with adjustable playback speed and pacing controls. |
| **`sgrn_dataset`** | Feature Extraction Tool | Decodes raw write-ahead logs against an SCL schema directly into structured CSV/Parquet datasets for ML pipelines. |
| **`datastore`** | Edge Persistence Engine | Local buffer orchestrating spooling and forwarding of telemetry to PostgreSQL or S3-compatible storage. |

---

## Primary Use Cases

### 1. Unified Multi-Protocol Edge Convergence
Factories frequently suffer from fragmented protocol silos (e.g. S7 for PLCs, Modbus for ancillary sensors, OPC-UA for SCADA). SGRN serves as a single edge hub: it ingests heterogeneous protocol streams, normalizes them into unified shadow memory, and projects this state outwards over standard REST, WebSocket, or OPC-UA endpoints.

### 2. Declarative PLC Integration (Zero Recompilation)
When an automation engineer updates a PLC DataBlock (e.g. adding a sensor or changing a UDT), traditional gateways require code updates, offset recalculations, and redeployments. With SGRN, the engineer updates the `.scl` schema. The runtime dynamically updates memory offsets, REST routes, and OPC-UA node hierarchies without recompiling or restarting edge services.

### 3. Edge Intelligence & Soft-PLC Control
Rather than streaming high-frequency data across network boundaries to a remote server for simple control decisions, `s7shell` executes AngelScript scripts directly against shadow memory. Scripts run with zero polling latency and batch writes into PDU-optimized network packets, enabling deterministic edge-level interlocks and closed-loop control.

### 4. Resilient Edge Historian & Replay
Under network partitions, SGRN buffers atomic process updates to high-throughput compressed write-ahead logs (`.zst`). When connectivity restores, data streams forward without gaps. Furthermore, past operational incidents can be replayed byte-for-byte into twin gateways using `sgrn_replay`.

### 5. Autonomous Plant Supervision & Trustworthy AI
Coupling SGRN's telemetry with `sgrn.ml` enables unsupervised, physics-constrained manifold anomaly detection. By extracting multi-dimensional process trajectories and calibrating non-conformity scores via split-conformal prediction, the system delivers early fault warnings (e.g., 18.0 hours before emergency shutdown) with provably bounded false-alarm rates.

### 6. Embedded Zero-Dependency Observability
Both `gateway` and `datastore` embed compiled Web UI dashboards (built with Svelte/React, compressed with Zstandard, and baked directly into C++ binary headers). Pointing a browser to the gateway IP provides instant, live operational visibility without external dependencies or cloud connectivity.

---

## Building SGRN

SGRN uses **CMake Presets** and a unified build environment managed with **Micromamba**.

### Prerequisites
- Linux x86_64 (Ubuntu 22.04+ or modern Linux distribution)
- [Micromamba](https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html) or Conda

### 1. Clone Repository
```bash
git clone <repository-url>
cd SGRN
```

> [!NOTE]
> SGRN does not require Git submodules. All third-party source dependencies are automatically downloaded directly into the local CMake cache (`~/.cache/CPM/`) on first configure via CMake Presets.

### 2. Initialize the Build Environment
The environment files in `micromamba/` provide compilers (GCC/Clang), Ninja, CMake, and all third-party C++ libraries in an isolated prefix:

```bash
# Create and activate the primary Linux development environment:
micromamba create -f micromamba/SGRN.yml
micromamba activate SGRN
```

### 3. Build & Install via CMake Presets

All builds must be run with the `SGRN` environment activated. Pre-configured presets are defined in `CMakePresets.json`:

```bash
# Configure using the Linux release preset:
cmake --preset linux-static-release

# Fast incremental install (builds our binaries and libraries into .dist/):
cmake --build .build/linux-static-release --target install-binaries

# Full install (binaries, headers, and dependencies):
cmake --build .build/linux-static-release --target install
```

> [!NOTE]
> For cross-compiling to **Windows** (`linux-to-win64`) or **ARM64** (`linux-to-arm64`), see the complete instructions in [**`documentation/BUILD.md`**](./documentation/BUILD.md).

---

## Quick Verification & Demos

### Run Interactive Gateway Demo
To verify the build, list and launch one of the built-in soft-PLC simulation skids:
```bash
python3 demos/gateway.py --list
python3 demos/gateway.py simple_skid
```
Access the embedded dashboard in your browser at `http://localhost:8000/`.

### Run Autonomous Supervision Benchmark
To run the full end-to-end plant anomaly detection experiment with live progress visualization:
```bash
python3 experiments/flare check
python3 experiments/flare run --output-dir scratch/quick_test
```

---

## License

This project is licensed under the **GNU Affero General Public License v3 (AGPLv3)**. See the [`LICENSE`](./LICENSE) file for details.
