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

try:
    from demos._port_utils import ensure_port_free, read_gateway_port
except ModuleNotFoundError:
    from _port_utils import ensure_port_free, read_gateway_port

ROOT = Path(__file__).resolve().parents[1]
SIMULATIONS = ROOT / "sgrn/lib/gateway/simulations"

BINARY_LOCATION = ""

if os.sys.platform == "win32":
    # Prefer the build output first on Windows: a crashed .dist binary may
    # still be locked by a zombie process, but the .build output is never
    # mapped by the demo runner so it is always replaceable.
    BINARY_LOCATION = ROOT / ".build/win64-native-static/sgrn/apps/gateway"
else:
    BINARY_LOCATION = ROOT / ".dist/linux-static-release"


@dataclass
class DemoRun:
    simulation: Path
    schema_file: Path
    config_path: Path
    as_scripts: list[Path]
    gateway_proc: subprocess.Popen
    shell_proc: subprocess.Popen


def binary(name: str) -> Path:
    exts = [".exe", ""] if sys.platform == "win32" else ["", ".exe"]
    names = [f"{name}{ext}" for ext in exts]

    candidate_dirs = (
        BINARY_LOCATION,
        ROOT / ".build/win64-native-static/sgrn/apps/gateway",
        ROOT / ".build/win64-native-static/sgrn/apps/s7shell",
        ROOT / ".dist/win64-native-static",
        ROOT / ".dist/win64-static",
        ROOT / ".dist/linux-static-release",
        ROOT / ".dist/linux-static",
        ROOT / ".prefix/win64-static/bin",
        ROOT / ".prefix/bin",
        ROOT / ".build/linux-static-release",
    )
    for directory in candidate_dirs:
        for n in names:
            candidate = directory / n
            if candidate.is_file() and (sys.platform == "win32" or os.access(candidate, os.X_OK)):
                return candidate
    for n in names:
        found = shutil.which(n)
        if found:
            return Path(found)
    raise FileNotFoundError(f"Cannot find {name}; build/install the binaries first.")


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


def _kill_stale_gateway(gateway_exe: Path) -> None:
    """Kill any lingering gateway processes left from a previous crashed run.

    On Windows a process that exits via an access violation stays in the
    process table (and holds a file lock on the EXE) until its parent reaps
    it.  Python's Popen.poll() sees returncode != None, so the demo script's
    finally block skips terminate() — leaving the zombie behind.

    We kill every process whose name is 'gateway.exe' (or 'gateway' on Linux)
    and whose executable resolves to any known gateway binary location, so
    stale processes are cleared regardless of which directory they were
    launched from.
    """
    try:
        import psutil  # type: ignore[import]
    except ImportError:
        return  # psutil not available — best-effort only

    # Build set of candidate exe paths to match against (both .dist and .build).
    target_name = "gateway.exe" if sys.platform == "win32" else "gateway"
    candidate_paths: set[str] = set()
    for d in (
        ROOT / ".dist/win64-native-static",
        ROOT / ".build/win64-native-static/sgrn/apps/gateway",
        ROOT / ".dist/linux-static-release",
        ROOT / ".build/linux-static-release/sgrn/apps/gateway",
    ):
        candidate = d / target_name
        if candidate.exists():
            candidate_paths.add(str(candidate.resolve()).lower())
    # Always include the exe we were given.
    candidate_paths.add(str(gateway_exe.resolve()).lower())

    killed: list[int] = []
    for proc in psutil.process_iter(["pid", "name", "exe"]):
        try:
            if proc.info.get("name", "").lower() != target_name.lower():
                continue
            exe = proc.info.get("exe") or ""
            if exe.lower() in candidate_paths or not candidate_paths:
                print(
                    f"[warn] Terminating stale gateway process PID {proc.pid} "
                    f"('{exe}').",
                    file=sys.stderr,
                )
                proc.kill()  # SIGKILL — don't wait for graceful exit
                killed.append(proc.pid)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    if killed:
        # Allow the OS to release file handles before we launch.
        time.sleep(1.5)


def startSimulation(
    selected_sim: Path, *, open_dashboard: bool = False,
    gateway_stdout=None, gateway_stderr=None, shell_stdout=None, shell_stderr=None,
) -> DemoRun:
    schema = selected_sim / "schema.scl"
    config = selected_sim / "gateway.json"
    scripts = sorted(p for p in selected_sim.glob("*.as") if p.name != "security.as")
    if not schema.is_file() or not config.is_file() or not scripts:
        raise FileNotFoundError(f"{selected_sim} needs schema.scl, gateway.json, and simulation scripts")
    gw_bin = binary("gateway")
    _kill_stale_gateway(gw_bin)
    ensure_port_free(read_gateway_port(config))
    gateway_args = [str(gw_bin), str(config)]
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
    parser.add_argument(
        "--run-without-simulation",
        action="store_true",
        help="launch the gateway only, without the s7shell soft-PLC simulation",
    )
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
    shell = None
    scripts: list = []
    if not args.run_without_simulation:
        shell = binary("s7shell")
        scripts = sorted(p for p in selected.glob("*.as") if p.name != "security.as")
        if not scripts:
            parser.error(f"No simulation scripts (*.as) found in {selected}")

    # Kill any leftover gateway processes from a previous crashed run so the
    # EXE file-lock is released before we try to launch a fresh instance.
    _kill_stale_gateway(gateway)

    command = [str(gateway), str(config)]
    if not args.no_gui:
        command.append("--gui")
    gw_port = read_gateway_port(config)
    print(f"Simulation: {selected.name}")
    if args.run_without_simulation:
        print("Running without simulation: s7shell will not be launched.")
    print(f"Dashboard: http://localhost:{gw_port}/" + (" (opened by gateway)" if not args.no_gui else ""))
    print("Starting gateway...")
    ensure_port_free(gw_port)
    processes: list[subprocess.Popen] = []

    # On Windows use CREATE_NEW_PROCESS_GROUP so that Ctrl-C sends
    # CTRL_BREAK_EVENT to the child's own process group rather than
    # propagating from the parent console (which would crash Python's
    # input() / readline machinery).
    popen_kwargs: dict = {}
    if sys.platform == "win32":
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP

    try:
        gw = subprocess.Popen(command, cwd=ROOT, **popen_kwargs)
        processes.append(gw)
        time.sleep(1)
        if gw.poll() is not None:
            raise RuntimeError(f"Gateway exited with status {gw.returncode}")
        if shell is not None:
            sim_cmd = [str(shell), *(p.name for p in scripts)]
            print("Starting soft PLC: " + ", ".join(p.name for p in scripts))
            plc = subprocess.Popen(sim_cmd, cwd=selected, **popen_kwargs)
            processes.append(plc)
        if args.no_gui and not args.no_browser:
            webbrowser.open(f"http://localhost:{gw_port}/")
        print("Press Ctrl+C to stop the experiment.")
        while all(p.poll() is None for p in processes):
            time.sleep(0.5)
        for p in processes:
            if p.poll() is not None:
                print(f"[demos/gateway.py] Process {p.args} exited with code {p.returncode}", file=sys.stderr)
        return next((p.returncode or 0 for p in processes if p.poll() is not None), 0)
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"Demo failed: {exc}", file=sys.stderr)
        return 1
    finally:
        for proc in reversed(processes):
            if proc.poll() is None:
                if sys.platform == "win32":
                    # Send CTRL_BREAK_EVENT to the process group for a graceful
                    # shutdown signal before falling back to hard terminate.
                    try:
                        import signal as _signal
                        proc.send_signal(_signal.CTRL_BREAK_EVENT)
                    except Exception:
                        proc.terminate()
                else:
                    proc.terminate()
        for proc in reversed(processes):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
