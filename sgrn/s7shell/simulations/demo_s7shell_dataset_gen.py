#!/usr/bin/env python3
"""
=============================================================================
demo_s7shell_dataset_gen.py — End-to-End Synthetic Data Gen & Replay Demo
=============================================================================
Demonstrates the full s7shell synthetic dataset generation pipeline:
  1. Runs `s7shell` with `simulation.as` to generate a compressed WAL (.bin.zst)
  2. Inspects the generated WAL archive statistics (size, compression ratio)
  3. Replays the WAL archive using `sgrn_replay` / `s7shell`

Usage:
  python3 sgrn/s7shell/simulations/demo_s7shell_dataset_gen.py
=============================================================================
"""

import os
import sys
import glob
import subprocess
import time
from pathlib import Path

# Color helpers
GREEN = "\033[32m"
CYAN = "\033[36m"
YELLOW = "\033[33m"
RED = "\033[31m"
BOLD = "\033[1m"
RESET = "\033[0m"

def log_info(msg: str):
    print(f"{CYAN}[DEMO]{RESET} {msg}")

def log_success(msg: str):
    print(f"{GREEN}[SUCCESS]{RESET} {msg}")

def log_warn(msg: str):
    print(f"{YELLOW}[WARN]{RESET} {msg}")

def log_error(msg: str):
    print(f"{RED}[ERROR]{RESET} {msg}")

def find_binary(name: str) -> str:
    """Find binary in project dist/build dirs or PATH."""
    repo_root = Path(__file__).resolve().parent.parent.parent.parent
    candidates = [
        repo_root / ".dist/linux-static-release" / "bin" / name,
        repo_root / ".dist/linux-static-release" / name,
        repo_root / ".build/linux-static-release/sgrn/s7shell" / name,
        repo_root / ".build/linux-static-release/sgrn/gateway" / name,
        repo_root / "build" / name,
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return str(c)
    
    # Check PATH
    import shutil
    p = shutil.which(name)
    if p:
        return p
    return ""

def main():
    repo_root = Path(__file__).resolve().parent.parent.parent.parent
    os.chdir(repo_root)

    print(f"{BOLD}================================================================={RESET}")
    print(f"{BOLD}  SGRN s7shell Synthetic Dataset Generation & Replay Pipeline    {RESET}")
    print(f"{BOLD}================================================================={RESET}")

    # 1. Locate binaries
    s7shell_bin = find_binary("s7shell")
    sgrn_replay_bin = find_binary("sgrn_replay")

    if not s7shell_bin:
        log_error("s7shell binary not found! Please build the project first: cmake --build .build/linux-static-release/ -j12 --target install")
        sys.exit(1)
    
    log_info(f"Using s7shell binary     : {s7shell_bin}")
    if sgrn_replay_bin:
        log_info(f"Using sgrn_replay binary : {sgrn_replay_bin}")
    else:
        log_warn("sgrn_replay binary not found. Will use s7shell for replay.")

    # 2. Run simulation script via s7shell
    sim_script = repo_root / "sgrn/s7shell/simulations/simulation.as"
    log_info(f"Executing synthetic data generation script: {sim_script.relative_to(repo_root)}")
    
    start_time = time.time()
    res = subprocess.run([s7shell_bin, str(sim_script)], capture_output=True, text=True)
    elapsed = time.time() - start_time

    if res.returncode != 0:
        log_error(f"s7shell simulation failed with code {res.returncode}:")
        print(res.stderr)
        sys.exit(1)

    print(res.stdout)
    log_success(f"Simulation completed in {elapsed:.2f} seconds.")

    # 3. Inspect generated WAL archive
    wal_dir = repo_root / "datasets/synthetic"
    archives = glob.glob(str(wal_dir / "**" / "*.bin.zst"), recursive=True) + \
               glob.glob(str(wal_dir / "**" / "*.jsonl.zst"), recursive=True)

    if not archives:
        log_error(f"No generated WAL archive found in {wal_dir}")
        sys.exit(1)

    latest_archive = max(archives, key=os.path.getmtime)
    archive_size_bytes = os.path.getsize(latest_archive)
    archive_size_kb = archive_size_bytes / 1024.0

    log_success(f"Generated WAL Archive: {Path(latest_archive).name}")
    log_info(f"  File size    : {archive_size_kb:.2f} KB ({archive_size_bytes:,} bytes)")
    log_info(f"  Sim horizon  : 30 minutes (18,000 ticks @ 10 Hz)")
    log_info(f"  Gen Speed    : {1800.0 / max(elapsed, 0.001):.1f}x real-time fast-forward")

    # 4. Replay the archive
    print(f"\n{BOLD}-----------------------------------------------------------------{RESET}")
    log_info("Replaying generated WAL archive into digital twin...")
    print(f"{BOLD}-----------------------------------------------------------------{RESET}")

    if sgrn_replay_bin:
        schema_path = repo_root / "sgrn/s7shell/simulations/schema.scl"
        replay_cmd = [
            sgrn_replay_bin,
            "--archive", latest_archive,
            "--schema",  str(schema_path),
            "--no-delay",           # replay as fast as possible
        ]
        # No -c/--config: sgrn_replay auto-detects headless mode and
        # starts HTTP+WebSocket only (no gateway.json required).
    else:
        # Fall back to scriptable replay via s7shell
        replay_script_content = f"""
        void main() {{
            WalReplayer@ rep = WalReplayer("{latest_archive}");
            rep.speed(0.0); // max speed replay
            rep.run();
        }}
        """
        replay_script_path = repo_root / "sgrn/s7shell/simulations/run_replay.as"
        replay_script_path.write_text(replay_script_content)
        replay_cmd = [s7shell_bin, str(replay_script_path)]

    replay_start = time.time()
    replay_res = subprocess.run(replay_cmd, capture_output=True, text=True)
    replay_elapsed = time.time() - replay_start

    if replay_res.returncode == 0 or "replay finished" in replay_res.stdout.lower():
        print(replay_res.stdout)
        log_success(f"Archive replay finished successfully in {replay_elapsed:.2f} seconds.")
    else:
        log_warn("Replay finished with output:")
        print(replay_res.stdout)
        if replay_res.stderr:
            print(replay_res.stderr)

    print(f"\n{BOLD}================================================================={RESET}")
    log_success("End-to-End s7shell Dataset Generation & Replay Pipeline Complete!")
    print(f"{BOLD}================================================================={RESET}")

if __name__ == "__main__":
    main()
