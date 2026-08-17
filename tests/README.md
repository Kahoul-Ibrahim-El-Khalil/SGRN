# SGRN Test Suite

Welcome to the centralized test suite for SGRN! This directory contains comprehensive tests to validate all components of the system, including the Gateway, S7Shell, SCL Compiler, and advanced data-flow mechanisms.

## Directory Layout

The tests are logically grouped by their target domain or technology stack:

- **`gateway/`**: Integration and unit tests targeting the SGRN Gateway's core capabilities. This includes REST endpoints, dual-WebSocket telemetry, and southbound protocol drivers (OPC UA, Modbus).
- **`scl/`**: Validation for the SCL (Siemens Control Language) schema compiler, focusing on data type resolution, endianness, and memory bounds.
- **`advanced/`**: Heavy stress tests and advanced scenarios like complex multi-dimensional array bounds checking and binary live snapshot synchronization.
- **`ts/`**: A comprehensive end-to-end (E2E) TypeScript suite powered by Bun. This spins up the Gateway and S7Shell in isolated environments and exhaustively tests the API, registry, policies, and dashboard interactions.
- **`cpp/`**: Native C++ test utilities and explorers (e.g., `opcua_explorer.cpp`).
- **`scripts/`**: Reusable AngelScript (`.as`) simulation logic and SCL (`.scl`) schemas injected as fixtures during test runs.

---

## The Interactive Test Runner (`test.py`)

To ensure maximum hygiene and ease of use, all tests are dispatched through the centralized `test.py` runner. It supports both interactive and CLI-driven executions, automatically managing dependencies and injecting simulation environments.

### Usage

**1. Interactive Menu:**
Running the script without any arguments opens an interactive menu where you can view and select available test suites.
```bash
./test.py
```

**2. CLI Execution:**
You can bypass the menu by specifying the test name directly. You can also optionally specify the target simulation name.
```bash
./test.py gateway-opcua gas_processing
```
*Tip: You can use `./test.py all` to run all Python suites and the TypeScript E2E suite consecutively.*

**3. Inspecting Tests (`--message`):**
Before running a test, you might want to know exactly what it tests and what its operational assumptions are (e.g., if it expects a live gateway or runs offline). 

Append `--message` to any test name to see its metadata:
```bash
./test.py gateway-rest --message
```
*Output:*
```text
==============================================
 🧪 TEST: gateway-rest [GATEWAY]
==============================================

Description:
   Offline integration tests for the SGRN Python bindings and REST API.

Assumptions:
   Runs a local HTTP server emulating the gateway. Does not require a live gateway.

Location: gateway/rest_api.py
==============================================
```

*Tip: You can use `./test.py all --message` to print out a comprehensive list of all tests, descriptions, and assumptions.*

---

## State Isolation

Tests that require spinning up a Gateway process are configured to be non-destructive. They dynamically allocate their state in `/tmp/gateway-state-{simulation_name}/`. The runner copies the required `security.as` and generates a virtual `gateway.json` internally, guaranteeing that running the tests will **never** mutate your original repository files.
