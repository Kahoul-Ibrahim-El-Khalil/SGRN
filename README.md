# SGRN

SGRN is a high-performance, modular C++ stack designed for industrial IoT, large scale data acquisition and storage, and digital twin applications. 

This project was developed as an implementation of the systems described in my Engineering and Master's Theses to tackle the growing complexity of edge-to-cloud industrial 
integrations.

## Target Audience & Main Objective

**Target Audience:** Industrial automation engineers, IoT integrators, and software architects building large-scale SCADA systems or Digital Twins who require robust, deterministic 
edge performance without restrictive vendor lock-in.

**Main Objective:** The primary goal of SGRN is to **automate the repetitive, error-prone tasks of PLC integration**.

Historically, mapping PLC memory required writing imperative code full of manual byte-offset calculations. SGRN shifts this entirely to a **declarative approach**. By using parsed SCL schemas, the system dynamically couples the PLC information model to the gateway. This eliminates the need to rewrite or recompile the edge middleware every time the automation engineer modifies the PLC logic.

## The Problem It Solves

Industrial automation systems often operate in isolated silos. Extracting data from PLCs (Programmable Logic Controllers) and bringing it into modern cloud or on-premise data stores typically involves complex, brittle, and expensive middleware. Existing solutions often struggle with:

1. **Performance at Scale:** Polling thousands of tags efficiently without overwhelming the PLC network stack.
2. **Data Consistency:** Ensuring that complex data structures (like UDTs and Strings) are read and written atomically without "torn reads" or race conditions.
3. **Flexibility:** Adapting to changing PLC schemas without requiring full recompilation or redeployment of the gateway.
4. **Security:** Providing granular, protocol-specific authorization and policy enforcement at the edge.

SGRN addresses these challenges by providing a unified, memory-safe gateway that maps PLC memory directly into a real-time Digital Twin, scriptable via AngelScript, and accessible via standard protocols like OPC-UA, HTTP/JSON, and WebSockets.

## Philosophy & Core Tenets

SGRN is not just a collection of scripts; it is engineered with a strict industrial philosophy aimed at stability, safety, and performance. These tenets are enumerated in detail in [`MANIFESTO.md`](./MANIFESTO.md); in brief, SGRN (1) protects the PLC via a shadow-memory digital twin that absorbs all IT reads, (2) enforces determinism and memory safety through C++ RAII and zero-allocation hot paths, (3) decouples the information model from the C++ binary via the `scl` schema parser, (4) pushes intelligence to the edge through a scriptable soft-PLC (`s7shell`), and (5) projects a single information model across protocols while serving embedded observability dashboards.

## Repository Structure & Core Modules

The stack is organized into highly cohesive, decoupled C++ modules.


### Internal Modules (`sgrn/`)

| Module | Utility | Architectural Value |
| :--- | :--- | :--- |
| **`scl`** | S7 Schema Parser | Parses native Siemens AWL/SCL datablock exports to build a dynamic Information Model. Eliminates hardcoded byte offsets in C++, allowing the gateway to adapt to PLC changes via JSON. |
| **`gateway`** | Multi-protocol Telemetry Hub | The passive edge server. Pulls PLC states into a mutex-protected "Digital Twin" to prevent network spam. Projects this model out via WebSockets and OPC-UA. Enforces granular security via a hot-reloadable `PolicyEngine`. |
| **`s7shell`** | Soft-PLC & Scripting Engine | An advanced runtime that exposes PLC data blocks to AngelScript. Enables edge-based closed-loop control and data aggregation with zero-latency shadow memory and automatic PDU-aware batching. |
| **`datastore`** | Persistence & REST API | A scalable, HTTP REST-like API orchestrating local data buffering and forwarding to time-series databases (PostgreSQL/TimescaleDB) or object storage (MinIO), ensuring high-frequency data survives network partitions. |
| **`scripting`** | VM Integration Layer | The reusable AngelScript sandbox binding C++ to the embedded scripting runtime. |
| **`core` & `utils`** | Foundational Libraries | Thread pools, logging, custom memory allocators, and basic data structures used universally across the SGRN stack. |
| **`sgrn_dataset` & `sgrn.ml`** | Telemetry Data & ML Pipeline | Streaming Zstd WAL feature extraction into CSV + Manifest, scikit-learn/XGBoost AutoML tournament training, ONNX export, and Digital Twin side-by-side replay. See [`documentation/PIPELINE_AND_TOOLS.md`](./documentation/PIPELINE_AND_TOOLS.md). |
| **`sdk`** | Client Libraries | C++ SDK for interacting with the SGRN gateway from external IT applications. |

## Getting Started

Building SGRN relies on a structured environment managed by Micromamba and CMake presets.

### 1. Clone & Initialize Submodules

SGRN builds on a standard Linux Machine
```bash
git clone <repository_url>
cd <repository_directory>
git submodule update --init --recursive
```

### 2. Setup Micromamba Environments

The project uses `micromamba` to ensure fully reproducible, isolated build environments across platforms.
Initialize the SGRN environment, for creating the environments the .yml files are in `micromamba/`, the main Linux environment is SGRN, the other ones in particular `SGRN-WIN64` and 
`SGRN-ARM64` are for cross compiling to windows and arm64 linux machines.

### 3. Build using CMake Presets

SGRN's CMake configuration provides predefined presets that orchestrate the build process for multiple targets simultaneously:

- **Linux** (Full build)
- **Windows** (Builds the gateway and tools, excluding the `datastore` which relies on Linux-specific tech)
- **ARM64** (Cross-compilation for edge devices like Raspberry Pi or industrial edge nodes)

To build using a preset:

```bash
# The build MUST run from inside the SGRN environment (it supplies the
# compilers, ninja and the target dependency libraries):
micromamba activate SGRN

cmake --preset <preset-name>

# Install everything (headers + binaries + deps):
cmake --build .build/{preset} --target install

# Install third-party system libraries (rarely needed, once per machine):
cmake --build .build/{preset} --target install-deps-binaries

# Or install incrementally — only our binaries/libs, skip headers and deps (fast):
cmake --build .build/{preset} --target install-binaries
```

> **Environment requirement.** Activating `SGRN` first is mandatory — not optional.
> All presets hardcode tools and dependency search paths into this environment
> (keyed off `$CONDA_PREFIX`). The build guards against forgetting this:
> running `cmake` with no conda environment active fails fast with a clear
> message telling you to run `micromamba activate SGRN`; activating a *different*
> env prints a warning. The cross-presets additionally require the `SGRN-WIN64`
> (Windows) and `SGRN-ARM64` (ARM64 Linux) environments for their target libraries.

for Build system details read `documentation/BUILD.md` and `documentation/build/subprojects.md`

### 4. UI:
Both the `datastore` and the `gateway` have their dashboards embedded in them and are served through HTTP, ideally both should run behind a reverse proxy for TLS termination and 
compression, there are python scripts that build the React and Svelte applications, compress the artifacts with `zstd` and generate C++ headers that contain the raw data of the web 
assets, which then the buildsystem embed in the binaries.

The `gateway` has its documentation in `markdown` and are included in the embedded web application.
## License

This project is licensed under the **GNU Affero General Public License v3 (AGPLv3)**.
Please see the `LICENSE` file for the full text of the license.





### 5.Use Cases:

5.1. Unified Edge Gateway: Multi-Protocol Convergence

    The Challenge: Traditional factories often operate in protocol silos. A single machine might use S7 for PLC control, Modbus for sensors, and OPC-UA for MES integration. 
    IT teams typically need to deploy and manage multiple middleware boxes just to get a unified view of the plant floor. 
    This creates data inconsistency and a high administrative overhead.

    The SGRN Solution: The gateway module acts as a single, unified edge node. It is protocol-agnostic at its core.

        Data Convergence: The gateway ingests data from diverse protocols (S7, OPC-UA, Modbus, HTTP, EthernetIP) and maps it all into a single, 
        real-time "Shadow Memory" (Digital Twin). This local in memory cache provides a normalized view of the entire machine state.

        Policy-Based Routing: The PolicyEngine within the gateway allows administrators to define granular access rules. 
        You can specify that WebSocket clients can read "all S7 data" but only "specific sensor data from Modbus," all without touching a line of C++ code.

        Protocol Projection: Once the data is normalized in the Digital Twin, the gateway automatically projects this unified state outwards. OPC-UA clients, HTTP REST queries, 
        and WebSocket subscribers all interact with the same consistent dataset. 
        The SGRN gateway becomes the single source of truth for the edge, eliminating protocol-specific translation layers.

5.2. Edge-to-Cloud Data Pipeline with Fault-Tolerant Storage

    The Challenge: Collecting high-frequency machine data (e.g., vibration data at 1kHz) and reliably sending it to the cloud poses a challenge. Network partitions are common on 
    industrial sites, leading to data loss, gaps in analytics, and significant headaches for data engineers. Furthermore, raw PLC data is often unstructured, requiring complex 
    transformation logic at the cloud level.

5.3. Declarative PLC Integration (The "No-Recompile" Strategy)

    The Challenge: This is SGRN's flagship use case. In a traditional SCADA environment, if an automation engineer adds a new "Temperature" tag to a PLC's memory block (UDT), the integration team must go into their middleware, recalculate byte offsets, update the code, and redeploy the entire gateway. This is slow, error-prone, and often causes production downtime.

    The SGRN Solution:

        Declarative Metadata: The scl parser dynamically ingests an exported SCL (Structured Control Language) file from the Siemens TIA Portal. This file contains the complete memory map of the PLC datablocks (including offset, type, and length).

        Dynamic Coupling: The parser binds this schema to the gateway's Digital Twin. The C++ binary is now agnostic to the PLC memory layout.

        Zero-Downtime Updates: To integrate a new PLC tag, an engineer simply performs the scl schema update. The gateway rebinds the memory map and instantly exposes the new data 
        via HTTP/JSON and OPC-UA nodes without recompiling or restarting the process. This decouples the automation lifecycle (PLC programming) from the IT lifecycle 
        (Edge Gateway management).

5.4. Edge Intelligence & Soft-PLC: Closed-Loop Control

    The Challenge: Sending all sensor data to a central cloud server to make decisions (e.g., "Stop the motor if temperature > 80°C") introduces unacceptable latency and represents a single point of failure. Industrial processes require deterministic, millisecond-level reaction times.

    The SGRN Solution: The s7shell module transforms the SGRN gateway into a "Soft-PLC" or intelligent edge node.

        Low-Latency Actions: AngelScripts execute directly against the "Shadow Memory" (Digital Twin). This means the script has zero-latency access to the latest PLC state without even polling the PLC network.

        Automatic Batching: When the script writes data back to the PLC (e.g., writing a new Setpoint), the s7shell intelligently batches these writes into PDU-optimized chunks, protecting the automation controller from being overwhelmed by thousands of individual write requests.

        Local Decision Making: This allows for closed-loop control to be performed at the edge. The gateway can shut down a conveyor or adjust a PID loop based on local sensor data, completely independent of the cloud, making the system highly resilient and responsive.

5.5. Out-of-the-Box Observability & Embedded UIs
    
    The Challenge: When deploying an edge gateway on a remote oil rig or production line, getting visibility into its status and the data it's collecting usually requires complex 
    cloud dashboards or expensive third-party HMI software. This adds cost and complexity to the initial setup.

    The SGRN Solution: The gateway and datastore modules are designed to be self-contained.

        Embedded Web Applications: Both modules serve embedded UI dashboards (built with Svelte and React). The web assets are compressed with zstd and compiled directly into the 
        binary.
        Instant Visibility: By simply pointing a browser to the gateway's IP address on the factory network, engineers get an instant, real-time view of the machine state, system health, and data flow without needing a cloud connection or additional software licenses.
