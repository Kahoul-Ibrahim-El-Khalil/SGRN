#!/usr/bin/env python3
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

GATEWAY_BIN = os.path.join(REPO_ROOT, ".build", "linux-static-release", "sgrn", "gateway", "gateway")
S7SHELL_BIN = os.path.join(REPO_ROOT, ".build", "linux-static-release", "sgrn", "s7shell", "s7shell")

TEST_REGISTRY = {
    "gateway-rest": {
        "path": "gateway/rest_api.py",
        "category": "gateway",
        "type": "offline",
        "description": "Offline integration tests for the SGRN Python bindings and REST API.",
        "assumptions": "Runs a local HTTP server emulating the gateway. Does not require a live gateway."
    },
    "gateway-websocket": {
        "path": "gateway/websocket.py",
        "category": "gateway",
        "type": "integration",
        "description": "Tests dual WebSocket telemetry performance and subscriptions.",
        "assumptions": "Requires a live gateway running with WebSocket enabled on port 8081."
    },
    "gateway-opcua": {
        "path": "gateway/opcua_discovery.py",
        "category": "gateway",
        "type": "integration",
        "description": "Explores OPC UA node discovery and reads Digital Twin DBs.",
        "assumptions": "Requires a live gateway with OPC UA enabled on port 4840."
    },
    "gateway-modbus": {
        "path": "gateway/modbus.py",
        "category": "gateway",
        "type": "integration",
        "description": "Tests Modbus TCP southbound protocol and mapping validation.",
        "assumptions": "Requires a live gateway configured with Modbus mapping."
    },
    "scl-dtypes": {
        "path": "scl/dtypes_endianness.py",
        "category": "scl",
        "type": "offline",
        "description": "Validates SCL schema compilation into correct data types and endianness.",
        "assumptions": "Offline test. Depends on the sgrn.bindings python package."
    },
    "scl-validation": {
        "path": "scl/schema_registry_validation.py",
        "category": "scl",
        "type": "offline",
        "description": "Tests the SCL compiler by converting a declarative .scl schema directly into the expected JSON registry format.",
        "assumptions": "Offline test. Requires the native 'sclc' compiler binary to be built."
    },
    "advanced-arrays": {
        "path": "advanced/array_support.py",
        "category": "advanced",
        "type": "offline",
        "description": "Tests complex nested arrays and multi-dimensional bounds checking.",
        "assumptions": "Offline test. Validates the memory layout engine."
    },
    "advanced-live-snapshot": {
        "path": "advanced/binary_live_snapshot.py",
        "category": "advanced",
        "type": "integration",
        "description": "Tests binary live snapshot synchronization and memory bounds.",
        "assumptions": "Requires a live gateway. Heavy stress test."
    },
    "ts-suite": {
        "path": "ts",
        "category": "suite",
        "type": "suite",
        "description": "Comprehensive TypeScript E2E suite covering Gateway, Policy Engine, Registry, and S7Shell.",
        "assumptions": "Requires Bun runtime. Automatically spawns isolated gateway and s7shell processes internally."
    }
}

@contextmanager
def GatewayFixture(simulation_name):
    """Spawns the SGRN Gateway and its AngelScript simulation for integration tests."""
    sim_name = simulation_name or "gas_processing"
    ts = int(time.time() * 1000)
    state_dir = f"/tmp/gateway-state-{sim_name}-{ts}"
    repo_sim_dir = os.path.join(REPO_ROOT, "sgrn", "gateway", "simulations", sim_name)
    
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
            "websocket": {"port": 8081}
        },
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
        if sim_proc: sim_proc.kill()
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
    print(f"\nAssumptions:")
    print(textwrap.indent(textwrap.fill(info['assumptions'], width=70), '   '))
    print(f"\nLocation: {info['path']}")
    print(f"==============================================\n")

def run_test(test_name, simulation_name=None):
    info = TEST_REGISTRY[test_name]
    
    print(f"\n==============================================")
    print(f"▶ Running {test_name} " + (f"(Simulation: {simulation_name})" if simulation_name else ""))
    print(f"==============================================\n")

    def _execute():
        if test_name == "ts-suite":
            cwd = os.path.join(TESTS_DIR, info["path"])
            res = subprocess.run(["bun", "test"], cwd=cwd)
            return res.returncode
        else:
            script_path = os.path.join(TESTS_DIR, info["path"])
            env = os.environ.copy()
            if simulation_name:
                env["SGRN_SIMULATION"] = simulation_name
            res = subprocess.run([sys.executable, script_path], env=env)
            return res.returncode

    if info["type"] == "integration":
        with GatewayFixture(simulation_name):
            returncode = _execute()
    else:
        returncode = _execute()

    print(f"\n==============================================")
    if returncode == 0:
        print(f"✅ {test_name} PASSED")
    else:
        print(f"❌ {test_name} FAILED (Exit Code: {returncode})")
    print(f"==============================================\n")
    return returncode == 0

def main():
    parser = argparse.ArgumentParser(description="SGRN Interactive Modular Test Runner")
    parser.add_argument("test", nargs="?", help="Test to run (e.g. gateway-opcua, all, ts-suite)")
    parser.add_argument("simulation", nargs="?", help="Optional simulation name to pass")
    parser.add_argument("--message", action="store_true", help="Print the test description and assumptions instead of running it")
    args = parser.parse_args()

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
        print("SGRN Interactive Modular Test Runner")
        print("-" * 40)
        
        categories = {}
        for k, v in TEST_REGISTRY.items():
            categories.setdefault(v["category"], []).append(k)
            
        print("Available Test Suites:")
        print("  1. all")
        idx = 2
        test_mapping = {1: "all"}
        
        for cat, tests in categories.items():
            print(f"\n[{cat.upper()}]")
            for t in tests:
                t_type = TEST_REGISTRY[t]["type"]
                icon = "🌐" if t_type == "integration" else "⚡" if t_type == "offline" else "📦"
                print(f"  {idx}. {icon} {t}")
                test_mapping[idx] = t
                idx += 1
        
        print("-" * 40)
        choice = input("Enter test name or number to run (or --message to view descriptions): ").strip()
        if not choice:
            return
            
        if choice.endswith("--message"):
            args.message = True
            choice = choice.replace("--message", "").strip()
            
        if choice.isdigit():
            idx_choice = int(choice)
            if idx_choice in test_mapping:
                args.test = test_mapping[idx_choice]
            else:
                args.test = choice
        else:
            args.test = choice
            
        if args.message:
            if args.test == "all":
                for name, info in TEST_REGISTRY.items():
                    print_message(name, info)
            elif args.test in TEST_REGISTRY:
                print_message(args.test, TEST_REGISTRY[args.test])
            return
        
        if args.test != "all" and TEST_REGISTRY.get(args.test, {}).get("type") == "integration":
            sim = input("Enter simulation name (optional, press Enter for 'gas_processing'): ").strip()
            if sim:
                args.simulation = sim
        elif args.test == "all":
            sim = input("Enter simulation name for integration tests (optional): ").strip()
            if sim:
                args.simulation = sim

    if args.test == "all":
        success = True
        for k in TEST_REGISTRY.keys():
            if not run_test(k, args.simulation):
                success = False
        sys.exit(0 if success else 1)
    elif args.test in TEST_REGISTRY:
        success = run_test(args.test, args.simulation)
        sys.exit(0 if success else 1)
    else:
        print(f"Unknown test: {args.test}")
        sys.exit(1)

if __name__ == "__main__":
    main()
