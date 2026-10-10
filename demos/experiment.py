#!/usr/bin/env python3
"""Launch an SGRN experiment (gateway + simulation + optional LLM client)."""

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
EXPERIMENTS = ROOT / "experiments"


@dataclass
class ExperimentRun:
    experiment: Path
    schema_file: Path
    config_path: Path
    as_scripts: list[Path]
    gateway_proc: subprocess.Popen
    shell_proc: subprocess.Popen
    llm_proc: subprocess.Popen | None = None


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


def discoverExperiments() -> list[Path]:
    return sorted(p for p in EXPERIMENTS.iterdir() if p.is_dir() and (p / "schema.scl").exists() and (p / "gateway.json").exists())


def resolveExperiment(choice: str | None = None) -> Path:
    experiments = discoverExperiments()
    if not experiments:
        raise FileNotFoundError(f"No experiments found under {EXPERIMENTS}")
    if choice is None:
        return experiments[0]
    if choice.isdigit() and 1 <= int(choice) <= len(experiments):
        return experiments[int(choice) - 1]
    for exp in experiments:
        if choice == exp.name or choice == str(exp):
            return exp
    raise ValueError(f"unknown experiment: {choice}")


def hasLLMClient(exp_dir: Path) -> bool:
    return (exp_dir / "llm_client.py").exists() or (exp_dir / "mock_llm.py").exists()


def getLLMScript(exp_dir: Path) -> str | None:
    if (exp_dir / "mock_llm.py").exists():
        return "mock_llm.py"
    if (exp_dir / "llm_client.py").exists():
        return "llm_client.py"
    return None


def startExperiment(
    selected_exp: Path, *, open_dashboard: bool = False, with_llm: bool = True,
    gateway_stdout=None, gateway_stderr=None, shell_stdout=None, shell_stderr=None, llm_stdout=None, llm_stderr=None,
) -> ExperimentRun:
    schema = selected_exp / "schema.scl"
    config = selected_exp / "gateway.json"
    scripts = sorted(p for p in selected_exp.glob("*.as") if p.name != "security.as")
    if not schema.is_file() or not config.is_file() or not scripts:
        raise FileNotFoundError(f"{selected_exp} needs schema.scl, gateway.json, and simulation scripts")
    
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
            [str(binary("s7shell")), *(p.name for p in scripts)], cwd=selected_exp,
            stdout=shell_stdout, stderr=shell_stderr,
        )
    except Exception:
        gateway_proc.terminate()
        gateway_proc.wait()
        raise
    
    llm_proc = None
    if with_llm and hasLLMClient(selected_exp):
        llm_script = getLLMScript(selected_exp)
        if llm_script == "llm_client.py":
            # Check if .env exists for real LLM client
            env_file = selected_exp / ".env"
            if not env_file.exists():
                print(f"[warn] No .env file in {selected_exp}; LLM client will not start. Copy .env.example and configure.")
            else:
                print(f"Starting LLM client...")
                llm_proc = subprocess.Popen(
                    [sys.executable, "llm_client.py"], cwd=selected_exp,
                    stdout=llm_stdout, stderr=llm_stderr,
                )
                time.sleep(1)
                if llm_proc.poll() is not None:
                    print(f"[warn] LLM client failed to start (status {llm_proc.returncode})")
                    llm_proc = None
        elif llm_script == "mock_llm.py":
            print(f"Starting mock LLM provider...")
            llm_proc = subprocess.Popen(
                [sys.executable, "-u", "mock_llm.py"], cwd=selected_exp,
                stdout=llm_stdout, stderr=llm_stderr,
            )
            time.sleep(1)
            if llm_proc.poll() is not None:
                print(f"[warn] Mock LLM provider failed to start (status {llm_proc.returncode})")
                llm_proc = None
    
    return ExperimentRun(selected_exp, schema, config, scripts, gateway_proc, shell_proc, llm_proc)


def cleanupProcesses(gateway_proc, shell_proc, llm_proc=None) -> None:
    for proc in (llm_proc, shell_proc, gateway_proc):
        if proc and proc.poll() is None:
            proc.terminate()
    for proc in (llm_proc, shell_proc, gateway_proc):
        if proc:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", nargs="?", help="experiment name or 1-based number")
    parser.add_argument("--list", action="store_true", help="list available experiments")
    parser.add_argument("--no-gui", action="store_true", help="serve the dashboard without a desktop app window")
    parser.add_argument("--no-browser", action="store_true", help="do not open a browser (headless mode)")
    parser.add_argument("--no-llm", action="store_true", help="do not start the LLM client (if experiment has one)")
    args = parser.parse_args()

    experiments = discoverExperiments()
    if args.list:
        for i, path in enumerate(experiments, 1):
            llm_script = getLLMScript(path)
            if llm_script == "mock_llm.py":
                llm_mark = " 🤖(mock)"
            elif llm_script == "llm_client.py":
                llm_mark = " 🤖(api)"
            else:
                llm_mark = ""
            print(f"{i:2}. {path.name}{llm_mark}")
        return 0
    if not experiments:
        parser.error(f"No experiments found under {EXPERIMENTS}")

    choice = args.experiment
    if choice is None:
        for i, path in enumerate(experiments, 1):
            llm_mark = " 🤖" if hasLLMClient(path) else ""
            print(f"{i:2}. {path.name}{llm_mark}")
        choice = input("Choose an experiment [1]: ").strip() or "1"
    selected = next((p for p in experiments if p.name == choice), None)
    if selected is None and choice.isdigit() and 1 <= int(choice) <= len(experiments):
        selected = experiments[int(choice) - 1]
    if selected is None:
        parser.error(f"Unknown experiment: {choice}")

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
    print(f"Experiment: {selected.name}")
    print(f"Dashboard: http://localhost:8000/" + (" (opened by gateway)" if not args.no_gui else ""))
    print("Starting gateway...")
    processes: list[subprocess.Popen] = []
    llm_proc = None
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
        
        # Start LLM client if experiment has one
        if not args.no_llm and hasLLMClient(selected):
            llm_script = getLLMScript(selected)
            if llm_script == "llm_client.py":
                env_file = selected / ".env"
                if env_file.exists():
                    print("Starting LLM client...")
                    llm_proc = subprocess.Popen([sys.executable, "llm_client.py"], cwd=selected)
                    processes.append(llm_proc)
                    time.sleep(1)
                    if llm_proc.poll() is not None:
                        print(f"[warn] LLM client exited early (status {llm_proc.returncode})")
                else:
                    print("[info] Skipping LLM client (no .env file found; copy .env.example and configure)")
            elif llm_script == "mock_llm.py":
                print("Starting mock LLM provider...")
                llm_proc = subprocess.Popen(
                    [sys.executable, "-u", "mock_llm.py"], 
                    cwd=selected,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1
                )
                processes.append(llm_proc)
                time.sleep(1)
                if llm_proc.poll() is not None:
                    print(f"[warn] Mock LLM provider exited early (status {llm_proc.returncode})")
                    llm_proc = None

        if args.no_gui and not args.no_browser:
            webbrowser.open("http://localhost:8000/")
        print("Press Ctrl+C to stop the experiment.")
        while all(p.poll() is None for p in processes):
            time.sleep(0.5)
        return next((p.returncode or 0 for p in processes if p.poll() is not None), 0)
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"Experiment failed: {exc}", file=sys.stderr)
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