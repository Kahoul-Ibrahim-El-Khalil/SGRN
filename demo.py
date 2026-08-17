#!/usr/bin/env python3

import os
import sys
import glob
import time
import json
import signal
import subprocess
import webbrowser
from dataclasses import dataclass
from pathlib import Path

# Paths relative to project root
BASE_DIR = Path(__file__).resolve().parent
SIMULATIONS_DIR = BASE_DIR / "sgrn" / "gateway" / "simulations"
BASE_CONFIG_PATH = BASE_DIR / "sgrn" / "gateway" / "configs" / "gateway.json"
GATEWAY_BIN = BASE_DIR / ".dist" / "linux-static-release" / "gateway"
S7SHELL_BIN = BASE_DIR / ".dist" / "linux-static-release" / "s7shell"
DASHBOARD_URL : str = "http://localhost:8000/"


@dataclass
class DemoRun:
    simulation: Path
    schema_file: Path
    config_path: Path
    as_scripts: list[Path]
    gateway_proc: subprocess.Popen
    shell_proc: subprocess.Popen

def printHeader(title):
    print(f"\n{'='*50}\n{title}\n{'='*50}")


def discover_simulations() -> list[Path]:
    if not SIMULATIONS_DIR.exists():
        raise FileNotFoundError(f"Simulations directory not found at {SIMULATIONS_DIR}")
    return sorted([d for d in SIMULATIONS_DIR.iterdir() if d.is_dir()])


def resolve_simulation(choice: str | None = None) -> Path:
    simulations = discover_simulations()
    if not simulations:
        raise FileNotFoundError("No simulations found.")
    if not choice:
        return simulations[0]
    if choice.isdigit():
        idx = int(choice) - 1
        if idx < 0 or idx >= len(simulations):
            raise ValueError(f"simulation index out of range: {choice}")
        return simulations[idx]
    for sim in simulations:
        if sim.name == choice or str(sim) == choice:
            return sim
    raise ValueError(f"unknown simulation: {choice}")


def prepare_simulation(selected_sim: Path) -> tuple[Path, list[Path], Path]:
    schema_file = selected_sim / "schema.scl"
    if not schema_file.exists():
        raise FileNotFoundError(f"{schema_file} does not exist.")

    as_scripts = [s for s in sorted(selected_sim.glob("*.as")) if s.name != "security.as"]
    sim_config = selected_sim / "gateway.json"
    if not sim_config.exists():
        sim_config = BASE_CONFIG_PATH
    return schema_file, as_scripts, sim_config


def start_simulation(
    selected_sim: Path,
    *,
    open_dashboard: bool = False,
    gateway_stdout=None,
    gateway_stderr=None,
    shell_stdout=None,
    shell_stderr=None,
) -> DemoRun:
    schema_file, as_scripts, sim_config = prepare_simulation(selected_sim)

    print(f"[demo] Starting SGRN Gateway with config {sim_config}...")
    gateway_proc = subprocess.Popen(
        [str(GATEWAY_BIN), str(sim_config)],
        cwd=str(BASE_DIR),
        stdout=gateway_stdout,
        stderr=gateway_stderr,
    )

    time.sleep(1)
    if gateway_proc.poll() is not None:
        raise RuntimeError("Gateway failed to start.")

    print("[demo] Starting s7shell...")
    s7shell_cmd = [str(S7SHELL_BIN)] + [s.name for s in as_scripts]
    shell_proc = subprocess.Popen(
        s7shell_cmd,
        cwd=str(selected_sim),
        stdout=shell_stdout,
        stderr=shell_stderr,
    )

    time.sleep(2)
    if open_dashboard:
        print(f"[demo] Opening web dashboard: {DASHBOARD_URL}")
        try:
            webbrowser.open(DASHBOARD_URL)
        except Exception as e:
            print(f"[demo] Could not open browser automatically: {e}")

    return DemoRun(
        simulation=selected_sim,
        schema_file=schema_file,
        config_path=sim_config,
        as_scripts=as_scripts,
        gateway_proc=gateway_proc,
        shell_proc=shell_proc,
    )


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


def stop_shell(shell_proc):
    if shell_proc and shell_proc.poll() is None:
        shell_proc.terminate()
        shell_proc.wait()


def main():
    if os.geteuid() != 0:
        print("[demo] This script requires root privileges to bind to S7 (102) and Modbus (502). Elevating...")
        os.execvp("sudo", ["sudo", sys.executable] + sys.argv)
        
    # 1. Discover Simulations
    simulations = discover_simulations()
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

    try:
        run = start_simulation(selected_sim, open_dashboard=True)
    except Exception as exc:
        print(f"Error: {exc}")
        sys.exit(1)

    printHeader(f"Simulation '{selected_sim.name}' is running!")
    print(f"Schema:  {run.schema_file}")
    print(f"Scripts: {[s.name for s in run.as_scripts]}")
    print("\nInstructions:")
    print(" - The gateway is now running and serving the S7/OPC-UA/HTTP/WebSocket endpoints.")
    print(" - The soft-PLC (s7shell) is executing the simulation scripts.")
    print(" - The web dashboard should have opened. If not, navigate to http://localhost:8000/")
    print("\nPress Ctrl+C to stop the simulation.")

    # 6. Wait for interrupt
    def signal_handler(sig, frame):
        cleanupProcesses(run.gateway_proc, run.shell_proc)
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    try:
        # Keep main thread alive
        while True:
            # Check if any process died unexpectedly
            if run.gateway_proc.poll() is not None:
                print("\n[demo] Gateway exited unexpectedly.")
                break
            if run.shell_proc.poll() is not None:
                print("\n[demo] s7shell exited unexpectedly.")
                break
            time.sleep(1)
    except KeyboardInterrupt:
        pass # Caught by signal handler
    finally:
        cleanupProcesses(run.gateway_proc, run.shell_proc)

if __name__ == "__main__":
    main()
