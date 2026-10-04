#!/usr/bin/env python3
"""Launch an SGRN gateway simulation and its embedded dashboard."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
import webbrowser
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIMULATIONS = ROOT / "sgrn/lib/gateway/simulations"


@dataclass
class DemoRun:
    simulation: Path
    schema_file: Path
    config_path: Path
    as_scripts: list[Path]
    gateway_proc: subprocess.Popen
    shell_proc: subprocess.Popen


def binary(name: str) -> Path:
    candidates = (
        ROOT / ".dist/linux-static-release" / name,
        ROOT / ".dist/linux-static" / name,
        ROOT / ".prefix/bin" / name,
        ROOT / ".build/linux-static-release" / name,
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which(name)
    if found:
        return Path(found)
    raise FileNotFoundError(f"Cannot find {name}; build/install the Linux binaries first.")


def discoverSimulations() -> list[Path]:
    return sorted(p for p in SIMULATIONS.iterdir() if p.is_dir() and (p / "schema.scl").exists())


def resolveSimulation(choice: str | None = None) -> Path:
    simulations = discoverSimulations()
    if not simulations:
        raise FileNotFoundError(f"No simulations found under {SIMULATIONS}")
    if choice is None:
        return simulations[0]
    if choice.isdigit() and 1 <= int(choice) <= len(simulations):
        return simulations[int(choice) - 1]
    for simulation in simulations:
        if choice == simulation.name or choice == str(simulation):
            return simulation
    raise ValueError(f"unknown simulation: {choice}")


def startSimulation(
    selected_sim: Path, *, open_dashboard: bool = False,
    gateway_stdout=None, gateway_stderr=None, shell_stdout=None, shell_stderr=None,
) -> DemoRun:
    schema = selected_sim / "schema.scl"
    config = selected_sim / "gateway.json"
    scripts = sorted(p for p in selected_sim.glob("*.as") if p.name != "security.as")
    if not schema.is_file() or not config.is_file() or not scripts:
        raise FileNotFoundError(f"{selected_sim} needs schema.scl, gateway.json, and simulation scripts")
    gateway_args = [str(binary("gateway")), str(config)]
    if open_dashboard:
        gateway_args.append("--gui")
    gateway_proc = subprocess.Popen(
        gateway_args,
        cwd=ROOT, stdout=gateway_stdout, stderr=gateway_stderr,
    )
    time.sleep(1)
    if gateway_proc.poll() is not None:
        raise RuntimeError(f"Gateway failed to start (status {gateway_proc.returncode})")
    try:
        shell_proc = subprocess.Popen(
            [str(binary("s7shell")), *(p.name for p in scripts)], cwd=selected_sim,
            stdout=shell_stdout, stderr=shell_stderr,
        )
    except Exception:
        gateway_proc.terminate()
        gateway_proc.wait()
        raise
    return DemoRun(selected_sim, schema, config, scripts, gateway_proc, shell_proc)


def stopShell(shell_proc) -> None:
    if shell_proc and shell_proc.poll() is None:
        shell_proc.terminate()
        shell_proc.wait()


def cleanupProcesses(gateway_proc, shell_proc) -> None:
    for proc in (shell_proc, gateway_proc):
        if proc and proc.poll() is None:
            proc.terminate()
    for proc in (shell_proc, gateway_proc):
        if proc:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulation", nargs="?", help="simulation name or 1-based number")
    parser.add_argument("--list", action="store_true", help="list available simulations")
    parser.add_argument("--no-gui", action="store_true", help="serve the dashboard without a desktop app window")
    parser.add_argument("--no-browser", action="store_true", help="do not open a browser (headless mode)")
    args = parser.parse_args()

    simulations = sorted(p for p in SIMULATIONS.iterdir() if p.is_dir() and (p / "schema.scl").exists())
    if args.list:
        for i, path in enumerate(simulations, 1):
            print(f"{i:2}. {path.name}")
        return 0
    if not simulations:
        parser.error(f"No simulations found under {SIMULATIONS}")

    choice = args.simulation
    if choice is None:
        for i, path in enumerate(simulations, 1):
            print(f"{i:2}. {path.name}")
        choice = input("Choose a simulation [1]: ").strip() or "1"
    selected = next((p for p in simulations if p.name == choice), None)
    if selected is None and choice.isdigit() and 1 <= int(choice) <= len(simulations):
        selected = simulations[int(choice) - 1]
    if selected is None:
        parser.error(f"Unknown simulation: {choice}")

    config = selected / "gateway.json"
    if not config.exists():
        parser.error(f"Missing gateway config: {config}")
    gateway = binary("gateway")
    shell = binary("s7shell")
    scripts = sorted(p for p in selected.glob("*.as") if p.name != "security.as")
    if not scripts:
        parser.error(f"No simulation scripts (*.as) found in {selected}")

    command = [str(gateway), str(config)]
    if not args.no_gui:
        command.append("--gui")
    print(f"Simulation: {selected.name}")
    print(f"Dashboard: http://localhost:8000/" + (" (opened by gateway)" if not args.no_gui else ""))
    print("Starting gateway...")
    processes: list[subprocess.Popen] = []
    try:
        gw = subprocess.Popen(command, cwd=ROOT)
        processes.append(gw)
        time.sleep(1)
        if gw.poll() is not None:
            raise RuntimeError(f"Gateway exited with status {gw.returncode}")
        sim_cmd = [str(shell), *(p.name for p in scripts)]
        print("Starting soft PLC: " + ", ".join(p.name for p in scripts))
        plc = subprocess.Popen(sim_cmd, cwd=selected)
        processes.append(plc)
        if args.no_gui and not args.no_browser:
            webbrowser.open("http://localhost:8000/")
        print("Press Ctrl+C to stop the experiment.")
        while all(p.poll() is None for p in processes):
            time.sleep(0.5)
        return next((p.returncode or 0 for p in processes if p.poll() is not None), 0)
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"Demo failed: {exc}", file=sys.stderr)
        return 1
    finally:
        for proc in reversed(processes):
            if proc.poll() is None:
                proc.terminate()
        for proc in reversed(processes):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
