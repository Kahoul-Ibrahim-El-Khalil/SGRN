#!/usr/bin/env python3
"""SGRN single test interface — every suite, one entry point.

Covers three worlds, grouped by what they verify:
  offline / code   — in-process behavior, no network (numpy dtypes, memory
                     layout, C++ unit tests, ...).
  integration      — live network API of a spawned gateway (REST, WebSocket,
                     OPC UA, Modbus, ...).
  suite            — whole projects at once (ctest, bun E2E).

Usage:
    python3 tests/test.py                  # interactive menu
    python3 tests/test.py <name> [sim]     # run one entry
    python3 tests/test.py all [sim]        # run everything runnable
    python3 tests/test.py --list           # list entries + status
    python3 tests/test.py <name> --message # describe one entry
"""
import os
import sys
import subprocess
import argparse
import textwrap
import json
import time
import shutil
import urllib.request
import urllib.error
from contextlib import contextmanager

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TESTS_DIR)

BUILD_DIR = os.path.join(REPO_ROOT, ".build", "linux-static-release")
GATEWAY_BIN = os.path.join(BUILD_DIR, "sgrn", "apps", "gateway", "gateway")
S7SHELL_BIN = os.path.join(BUILD_DIR, "sgrn", "apps", "s7shell", "s7shell")

# surface: what the verdict actually covers — network API vs code behavior.
TEST_REGISTRY = {
    "gateway-rest": {
        "path": "gateway/rest_api.py",
        "category": "gateway",
        "type": "offline",
        "surface": "client lib + local loopback HTTP",
        "description": "Offline integration tests for the SGRN Python bindings and REST API.",
        "assumptions": "Runs a local HTTP server emulating the gateway. Does not require a live gateway."
    },
    "gateway-websocket": {
        "path": "gateway/websocket.py",
        "category": "gateway",
        "type": "integration",
        "surface": "WebSocket API over the network",
        "description": "Tests dual WebSocket telemetry performance and subscriptions.",
        "assumptions": "Requires a live gateway running with WebSocket enabled at /ws on the HTTP port (8080)."
    },
    "gateway-opcua": {
        "path": "gateway/opcua_discovery.py",
        "category": "gateway",
        "type": "integration",
        "surface": "OPC UA API over the network",
        "description": "Explores OPC UA node discovery and reads Digital Twin DBs.",
        "assumptions": "Requires a live gateway with OPC UA enabled on port 4840."
    },
    "gateway-modbus": {
        "path": "gateway/modbus.py",
        "category": "gateway",
        "type": "integration",
        "surface": "Modbus TCP API over the network",
        "description": "Tests Modbus TCP southbound protocol and mapping validation.",
        "assumptions": "Requires a live gateway configured with Modbus mapping."
    },
    "scl-dtypes": {
        "path": "scl/dtypes_endianness.py",
        "category": "scl",
        "type": "offline",
        "surface": "in-process code (NumPy)",
        "description": "Validates SCL schema compilation into correct data types and endianness.",
        "assumptions": "Offline test. Depends on the sgrn/python package."
    },
    "scl-validation": {
        "path": "scl/schema_registry_validation.py",
        "category": "scl",
        "type": "offline",
        "surface": "sclc tool behavior",
        "description": "Tests the SCL compiler by converting a declarative .scl schema directly into the expected JSON registry format.",
        "assumptions": "Offline test. Requires the native 'sclc' compiler binary to be built."
    },
    "advanced-arrays": {
        "path": "advanced/array_support.py",
        "category": "advanced",
        "type": "offline",
        "surface": "in-process code (memory layout)",
        "description": "Tests complex nested arrays and multi-dimensional bounds checking.",
        "assumptions": "Offline test. Validates the memory layout engine."
    },
    "advanced-live-snapshot": {
        "path": "advanced/binary_live_snapshot.py",
        "category": "advanced",
        "type": "integration",
        "surface": "binary snapshot API over the network",
        "description": "Tests binary live snapshot synchronization and memory bounds.",
        "assumptions": "Requires a live gateway. Heavy stress test."
    },
    "ts-suite": {
        "path": "ts",
        "category": "suite",
        "type": "suite",
        "surface": "network API end-to-end (HTTP/WS/OPC-UA/S7)",
        "description": "Whole TypeScript E2E suite in one go (all files below).",
        "assumptions": "Requires Bun runtime. Automatically spawns isolated gateway and s7shell processes internally."
    },
    "ts-dashboard": {
        "path": "ts",
        "file": "tests/dashboard.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "dashboard UI over HTTP",
        "description": "Web dashboard serving and endpoints.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-data": {
        "path": "ts",
        "file": "tests/data.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "data REST API",
        "description": "Data endpoint integration tests.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-gateway-api": {
        "path": "ts",
        "file": "tests/gateway-api.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "REST API surface (registry/data/memory/policy)",
        "description": "Comprehensive Gateway API tests over HTTP.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-policy": {
        "path": "ts",
        "file": "tests/policy.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "policy engine API",
        "description": "Policy engine integration tests.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-registry": {
        "path": "ts",
        "file": "tests/registry.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "schema registry API",
        "description": "Registry endpoint integration tests.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-s7shell": {
        "path": "ts",
        "file": "tests/s7shell.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "s7shell CLI binary",
        "description": "S7Shell CLI tool tests (binary, help, schema load).",
        "assumptions": "Requires Bun runtime and a built s7shell binary."
    },
    "ts-websocket": {
        "path": "ts",
        "file": "tests/websocket.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "WebSocket telemetry API",
        "description": "WebSocket telemetry tests (subscriptions, heartbeats, limits).",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-southbound-s7": {
        "path": "ts",
        "file": "tests/southbound-s7.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "S7 southbound views over HTTP",
        "description": "S7 adapter behavior as seen through the northbound API.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-southbound-modbus": {
        "path": "ts",
        "file": "tests/southbound-modbus.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "Modbus mapping views over HTTP",
        "description": "Modbus adapter behavior as seen through the northbound API.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-southbound-opcua": {
        "path": "ts",
        "file": "tests/southbound-opcua.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "OPC-UA views over HTTP/WebSocket",
        "description": "OPC-UA adapter behavior as seen through the northbound API.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-southbound-ethernetip": {
        "path": "ts",
        "file": "tests/southbound-ethernetip.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "EtherNet/IP views over HTTP/WebSocket",
        "description": "EtherNet/IP adapter behavior as seen through the northbound API.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "ts-southbound-integration": {
        "path": "ts",
        "file": "tests/southbound-integration.test.ts",
        "category": "ts",
        "type": "suite",
        "surface": "cross-protocol behavior (aggregation, errors, load)",
        "description": "Multi-protocol aggregation, error handling, config validation, reliability.",
        "assumptions": "Requires Bun runtime. Spawns its own gateway internally."
    },
    "cpp": {
        "path": "<ctest>",
        "category": "cpp",
        "type": "suite",
        "surface": "in-process code (native unit tests)",
        "description": "Native C++ unit tests via CTest (compression, OPC-UA projection, adapter/twin ports, WAL replay, init scripts, schema reload hook).",
        "assumptions": "Requires a configured CMake build dir (.build/linux-static-release). No PLC hardware or network needed."
    }
}

CATEGORY_ORDER = ["cpp", "gateway", "scl", "advanced", "ts", "suite"]


def check_prereqs(test_name):
    """Return (ok, reason). Unavailable entries are SKIPPED, never failed."""
    info = TEST_REGISTRY[test_name]
    if test_name == "cpp":
        if not os.path.isdir(BUILD_DIR):
            return False, f"missing CMake build dir {BUILD_DIR} (configure first)"
        return True, ""
    if test_name == "ts-suite" or test_name.startswith("ts-"):
        if shutil.which("bun") is None:
            return False, "bun not on PATH"
        return True, ""
    if info["type"] == "integration":
        if not os.path.isfile(GATEWAY_BIN):
            return False, f"missing gateway binary {GATEWAY_BIN} (build first)"
        return True, ""
    return True, ""


@contextmanager
def GatewayFixture(simulation_name):
    """Spawns the SGRN Gateway and its AngelScript simulation for integration tests."""
    sim_name = simulation_name or "gas_processing"
    ts = int(time.time() * 1000)
    state_dir = f"/tmp/gateway-state-{sim_name}-{ts}"
    repo_sim_dir = os.path.join(REPO_ROOT, "sgrn", "lib", "gateway", "simulations", sim_name)

    os.makedirs(state_dir, exist_ok=True)

    policy_path = os.path.join(state_dir, "security.as")
    test_config_path = os.path.join(state_dir, "gateway.json")

    repo_policy = os.path.join(repo_sim_dir, "security.as")
    if os.path.exists(repo_policy):
        shutil.copy2(repo_policy, policy_path)
    else:
        with open(policy_path, "w") as f:
            f.write("void setup() { http().allow(); opcua().allow(); websocket().allow(); }\n")

    config = {
        "listen": {
            "s7": {"port": 8102},
            "opcua": {"port": 4840},
            "http": {"port": 8080},
            # WebSocket shares the HTTP listener at /ws (same port).
            "websocket": {"port": 8080}
        },
        # Pin the gateway's own archive/state dir under /tmp: the product
        # default is CWD-relative ("./gateway-state") and must never land
        # in the source tree during tests.
        "state_dir": os.path.join(state_dir, "gateway-state"),
        "schema": os.path.join(repo_sim_dir, "schema.scl"),
        "security_script": policy_path
    }

    with open(test_config_path, "w") as f:
        json.dump(config, f, indent=2)

    print(f"🔧 Starting Gateway with simulation '{sim_name}'...")
    gateway_proc = subprocess.Popen([GATEWAY_BIN, test_config_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    sim_proc = None
    sim_script = os.path.join(repo_sim_dir, "simulation.as")
    if os.path.exists(sim_script):
        print(f"🔧 Starting S7Shell simulation script...")
        sim_proc = subprocess.Popen([S7SHELL_BIN, sim_script], cwd=repo_sim_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Wait for gateway to be ready
    ready = False
    for _ in range(50):
        try:
            res = urllib.request.urlopen("http://127.0.0.1:8080/endpoints", timeout=1)
            if res.getcode() == 200:
                ready = True
                break
        except Exception:
            pass
        time.sleep(0.1)

    if not ready:
        gateway_proc.kill()
        if sim_proc:
            sim_proc.kill()
        raise RuntimeError("Gateway failed to start or bind to port 8080.")

    print("🚀 Gateway is online. Running test...")
    try:
        yield
    finally:
        print("🛑 Tearing down Gateway...")
        gateway_proc.terminate()
        if sim_proc:
            sim_proc.terminate()
        gateway_proc.wait()
        if sim_proc:
            sim_proc.wait()
        shutil.rmtree(state_dir, ignore_errors=True)


def print_message(test_name, info):
    print(f"\n==============================================")
    print(f" 🧪 TEST: {test_name} [{info['category'].upper()}] ({info['type'].upper()})")
    print(f"==============================================")
    print(f"\nDescription:")
    print(textwrap.indent(textwrap.fill(info['description'], width=70), '   '))
    print(f"\nVerifies: {info.get('surface', info['type'])}")
    print(f"\nAssumptions:")
    print(textwrap.indent(textwrap.fill(info['assumptions'], width=70), '   '))
    print(f"\nLocation: {info['path']}")
    ok, reason = check_prereqs(test_name)
    print(f"Status: {'ready' if ok else 'UNAVAILABLE — ' + reason}")
    print(f"==============================================\n")


def run_test(test_name, simulation_name=None):
    """Run one entry. Returns a (status, seconds) tuple; streams live output."""
    info = TEST_REGISTRY[test_name]
    started = time.time()

    print(f"\n==============================================")
    print(f"▶ Running {test_name} " + (f"(Simulation: {simulation_name})" if simulation_name else ""))
    print(f"  verifies: {info.get('surface', info['type'])}")
    print(f"==============================================\n")

    def _execute():
        if test_name == "ts-suite" or test_name.startswith("ts-"):
            cwd = os.path.join(TESTS_DIR, info["path"])
            cmd = ["bun", "test"] + ([info["file"]] if "file" in info else [])
            res = subprocess.run(cmd, cwd=cwd)
            return res.returncode
        elif test_name == "cpp":
            res = subprocess.run(
                ["ctest", "--test-dir", BUILD_DIR, "--output-on-failure"],
                cwd=REPO_ROOT)
            return res.returncode
        else:
            script_path = os.path.join(TESTS_DIR, info["path"])
            env = os.environ.copy()
            if simulation_name:
                env["SGRN_SIMULATION"] = simulation_name
            res = subprocess.run([sys.executable, script_path], env=env)
            return res.returncode

    try:
        if info["type"] == "integration":
            with GatewayFixture(simulation_name):
                returncode = _execute()
        else:
            returncode = _execute()
    except RuntimeError as e:
        print(f"❌ {test_name} FAILED to start fixture: {e}")
        return ("error", time.time() - started)

    elapsed = time.time() - started
    status = "pass" if returncode == 0 else "fail"
    print(f"\n{('✅' if status == 'pass' else '❌')} {test_name} {status.upper()} ({elapsed:.1f}s)")
    return (status, elapsed)


def parse_selection(choice, mapping):
    """'1,3-5,all,cpp' -> ordered entry names. 'q'/empty -> []."""
    choice = choice.strip()
    if not choice or choice.lower() in ("q", "quit", "exit"):
        return []
    names = list(TEST_REGISTRY.keys())
    picked = []
    for token in choice.replace(",", " ").split():
        token = token.strip().lower()
        if token == "all":
            return names
        if token in TEST_REGISTRY and token not in picked:
            picked.append(token)
            continue
        if "-" in token:
            try:
                lo, hi = token.split("-", 1)
                for i in range(int(lo), int(hi) + 1):
                    if i in mapping and mapping[i] not in picked:
                        picked.append(mapping[i])
                continue
            except ValueError:
                pass
        if token.isdigit() and int(token) in mapping:
            if mapping[int(token)] not in picked:
                picked.append(mapping[int(token)])
            continue
        print(f"Unknown selection: {token}")
        return []
    return picked


def menu_order():
    """Canonical display order: categories first, registry order within."""
    cats = {}
    for k, v in TEST_REGISTRY.items():
        cats.setdefault(v["category"], []).append(k)
    ordered = []
    for cat in CATEGORY_ORDER + [c for c in cats if c not in CATEGORY_ORDER]:
        ordered.extend(cats.get(cat, []))
    return ordered


def print_menu():
    """Grouped menu with per-entry readiness. Returns index -> name mapping."""
    print("\nSGRN Test Runner — one interface for every suite")
    print("offline = in-process code behavior · integration/suite = live network API")
    print("-" * 70)
    mapping = {}
    prev_cat = None
    for idx, t in enumerate(menu_order(), start=1):
        info = TEST_REGISTRY[t]
        if info["category"] != prev_cat:
            print(f"\n[{info['category'].upper()}]")
            prev_cat = info["category"]
        ok, reason = check_prereqs(t)
        tag = "ready" if ok else f"UNAVAILABLE ({reason})"
        print(f"  {idx:2d}. {t:24s} [{info['type']:11s}] {info.get('surface', '')}")
        print(f"      └─ {tag}")
        mapping[idx] = t
    print("-" * 70)
    return mapping


def print_summary(results):
    """results: list of (name, status, seconds)."""
    print("\n==============================================")
    print(" RESULTS (network API vs code behavior)")
    print("==============================================")
    by_type = {}
    for name, status, secs in results:
        by_type.setdefault(TEST_REGISTRY[name]["type"], []).append((name, status, secs))
    totals = {"pass": 0, "fail": 0, "skip": 0, "error": 0}
    total_time = 0.0
    for kind in ("offline", "integration", "suite"):
        if kind not in by_type:
            continue
        label = {"offline": "code behavior (in-process)",
                 "integration": "network API (live gateway)",
                 "suite": "whole suites"}[kind]
        print(f"\n  {label}:")
        for name, status, secs in by_type[kind]:
            totals[status] += 1
            total_time += secs
            icon = {"pass": "✅", "fail": "❌", "skip": "⏭️", "error": "🔥"}[status]
            print(f"    {icon} {name:24s} {status.upper():5s} ({secs:.1f}s) — {TEST_REGISTRY[name].get('surface', '')}")
    print(f"\n  {totals['pass']} passed, {totals['fail']} failed, "
          f"{totals['skip']} skipped, {totals['error']} errors in {total_time:.1f}s")
    print("==============================================\n")
    return totals["fail"] == 0 and totals["error"] == 0


def run_selection(names, simulation_name=None):
    results = []
    for name in names:
        ok, reason = check_prereqs(name)
        if not ok:
            print(f"\n⏭️  {name} SKIPPED — {reason}")
            results.append((name, "skip", 0.0))
            continue
        status, secs = run_test(name, simulation_name)
        results.append((name, status, secs))
    return print_summary(results)


def main():
    parser = argparse.ArgumentParser(description="SGRN single test interface: every suite, one entry point")
    parser.add_argument("test", nargs="?", help="Test name, number, 'all', or comma/range selection (e.g. 1,3-5)")
    parser.add_argument("simulation", nargs="?", help="Optional simulation name to pass")
    parser.add_argument("--message", action="store_true", help="Print the test description and assumptions instead of running it")
    parser.add_argument("--list", action="store_true", help="List all entries with readiness and exit")
    args = parser.parse_args()

    if args.list:
        mapping = print_menu()
        _ = mapping
        return

    if args.message and args.test:
        if args.test == "all":
            for name, info in TEST_REGISTRY.items():
                print_message(name, info)
        elif args.test in TEST_REGISTRY:
            print_message(args.test, TEST_REGISTRY[args.test])
        else:
            print(f"Unknown test: {args.test}")
        return
    elif args.message:
        print("Please specify a test name or 'all' with --message flag.")
        return

    if not args.test:
        mapping = print_menu()
        try:
            choice = input("Select (numbers/ranges/names, 'all', or 'q'): ").strip()
        except EOFError:
            print()
            return
        if choice.endswith("--message"):
            args.message = True
            choice = choice.replace("--message", "").strip()
        names = parse_selection(choice, mapping)
        if args.message:
            for name in names or TEST_REGISTRY.keys():
                print_message(name, TEST_REGISTRY[name])
            return
        if not names:
            return
        if any(TEST_REGISTRY[n]["type"] == "integration" for n in names) and not args.simulation:
            try:
                sim = input("Simulation for integration tests (Enter for default): ").strip()
            except EOFError:
                sim = ""
                print()
            if sim:
                args.simulation = sim
        ok = run_selection(names, args.simulation)
        sys.exit(0 if ok else 1)

    if args.test == "all":
        ok = run_selection(list(TEST_REGISTRY.keys()), args.simulation)
        sys.exit(0 if ok else 1)
    elif args.test in TEST_REGISTRY:
        ok = run_selection([args.test], args.simulation)
        sys.exit(0 if ok else 1)
    else:
        # maybe a bare selection like "1,3" passed positionally (menu numbering)
        mapping = {i + 1: n for i, n in enumerate(menu_order())}
        names = parse_selection(args.test, mapping)
        if not names:
            print(f"Unknown test: {args.test}")
            sys.exit(1)
        ok = run_selection(names, args.simulation)
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
