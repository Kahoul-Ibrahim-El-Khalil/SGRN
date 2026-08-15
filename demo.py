#!/usr/bin/env python3

import os
import sys
import glob
import time
import json
import signal
import subprocess
import webbrowser
from pathlib import Path

# Paths relative to project root
BASE_DIR = Path(__file__).resolve().parent
SIMULATIONS_DIR = BASE_DIR / "sgrn" / "gateway" / "simulations"
BASE_CONFIG_PATH = BASE_DIR / "sgrn" / "gateway" / "configs" / "gateway.json"
GATEWAY_BIN = BASE_DIR / ".dist" / "linux-static-release" / "gateway"
S7SHELL_BIN = BASE_DIR / ".dist" / "linux-static-release" / "s7shell"
DASHBOARD_URL : str = "http://localhost:8000/"

def printHeader(title):
    print(f"\n{'='*50}\n{title}\n{'='*50}")

def cleanupProcesses(gateway_proc, shell_proc):
    print("\n[demo] Shutting down simulation...")
    if shell_proc and shell_proc.poll() is None:
        shell_proc.terminate()
    if gateway_proc and gateway_proc.poll() is None:
        gateway_proc.terminate()
    
    # Wait for them to actually terminate
    if shell_proc: shell_proc.wait()
    if gateway_proc: gateway_proc.wait()
    print("[demo] Shutdown complete.")

def main():
    if os.geteuid() != 0:
        print("[demo] This script requires root privileges to bind to S7 (102) and Modbus (502). Elevating...")
        os.execvp("sudo", ["sudo", sys.executable] + sys.argv)
        
    if not SIMULATIONS_DIR.exists():
        print(f"Error: Simulations directory not found at {SIMULATIONS_DIR}")
        sys.exit(1)
        
    # 1. Discover Simulations
    simulations = sorted([d for d in SIMULATIONS_DIR.iterdir() if d.is_dir()])
    if not simulations:
        print("Error: No simulations found.")
        sys.exit(1)
        
    printHeader("SGRN Post-Compilation Demo")
    print("Available Simulations:")
    for idx, sim in enumerate(simulations, 1):
        print(f"  {idx}. {sim.name}")
        
    choice = input("\nSelect a simulation [1]: ").strip()
    if not choice:
        choice = "1"
        
    try:
        sim_idx = int(choice) - 1
        if sim_idx < 0 or sim_idx >= len(simulations):
            raise ValueError
        selected_sim = simulations[sim_idx]
    except ValueError:
        print("Invalid choice.")
        sys.exit(1)

    print(f"\n[demo] Loading simulation: {selected_sim.name}")
    
    schema_file = selected_sim / "schema.scl"
    if not schema_file.exists():
        print(f"Error: {schema_file} does not exist.")
        sys.exit(1)
        
    as_scripts = [s for s in sorted(selected_sim.glob("*.as")) if s.name != "security.as"]
    if not as_scripts:
        print(f"Warning: No AngelScript (*.as) files found in {selected_sim}")

    # 2. Setup config for Gateway
    sim_config = selected_sim / "gateway.json"
    if not sim_config.exists():
        print(f"Warning: No gateway.json found in {selected_sim}, falling back to default.")
        sim_config = BASE_CONFIG_PATH

    # 3. Start Gateway
    print(f"[demo] Starting SGRN Gateway with config {sim_config}...")
    gateway_proc = subprocess.Popen([str(GATEWAY_BIN), str(sim_config)], cwd=str(BASE_DIR))
    
    time.sleep(1) # Give it a moment to initialize ports
    if gateway_proc.poll() is not None:
        print("Error: Gateway failed to start.")
        sys.exit(1)

    # 4. Start s7shell
    print("[demo] Starting s7shell...")
    s7shell_cmd = [str(S7SHELL_BIN)] + [s.name for s in as_scripts]
    shell_proc = subprocess.Popen(s7shell_cmd, cwd=str(selected_sim))

    time.sleep(2) # Give shell time to connect and execute

    # 5. Open Web Browser
    dashboard_url = DASHBOARD_URL
    print(f"[demo] Opening web dashboard: {dashboard_url}")
    try:
        webbrowser.open(dashboard_url)
    except Exception as e:
        print(f"[demo] Could not open browser automatically: {e}")

    printHeader(f"Simulation '{selected_sim.name}' is running!")
    print(f"Schema:  {schema_file}")
    print(f"Scripts: {[s.name for s in as_scripts]}")
    print("\nInstructions:")
    print(" - The gateway is now running and serving the S7/OPC-UA/HTTP/WebSocket endpoints.")
    print(" - The soft-PLC (s7shell) is executing the simulation scripts.")
    print(" - The web dashboard should have opened. If not, navigate to http://localhost:8000/")
    print("\nPress Ctrl+C to stop the simulation.")

    # 6. Wait for interrupt
    def signal_handler(sig, frame):
        cleanupProcesses(gateway_proc, shell_proc)
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    try:
        # Keep main thread alive
        while True:
            # Check if any process died unexpectedly
            if gateway_proc.poll() is not None:
                print("\n[demo] Gateway exited unexpectedly.")
                break
            if shell_proc.poll() is not None:
                print("\n[demo] s7shell exited unexpectedly.")
                break
            time.sleep(1)
    except KeyboardInterrupt:
        pass # Caught by signal handler
    finally:
        cleanupProcesses(gateway_proc, shell_proc)

if __name__ == "__main__":
    main()
