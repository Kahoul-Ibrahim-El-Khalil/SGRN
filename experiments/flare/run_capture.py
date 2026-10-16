#!/usr/bin/env python3
"""experiments/flare/runCapture.py — Stage 1: WAL capture for FLARE_BLOCKED scenario.

Runs an isolated copy of simulation.as (Day 31, Hour 3: fault_code = 9) connected to
an isolated gateway process capturing into .jsonl.zst.

Produces:
  experiments/flare/flare_history.jsonl.zst
  experiments/flare/meta.json
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
SIM_DIR = ROOT / "sgrn/lib/gateway/simulations/gas_processing"

from config_loader import loadConfig, getConfigPath

CONFIG_PATH = getConfigPath(Path(__file__).parent)


def findBinary(name: str) -> Path:
    app_name = {"sgrn_replay": "replay"}.get(name, name)
    paths = (
        ROOT / ".dist/linux-static-release" / name,
        ROOT / ".dist/linux-static" / name,
        ROOT / ".prefix/bin" / name,
        ROOT / ".build/linux-static-release" / name,
        ROOT / ".build/linux-static-release/sgrn/apps" / app_name / name,
    )
    for path in paths:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    raise FileNotFoundError(f"Cannot find binary {name}; check .dist/linux-static-release/")


def waitHttp(url: str, proc: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"Process exited early with status {proc.returncode}")
        try:
            with urllib.request.urlopen(url, timeout=1):
                return
        except (urllib.error.URLError, TimeoutError):
            time.sleep(0.2)
    raise TimeoutError(f"Gateway did not become ready at {url}")


def stopProcess(proc: subprocess.Popen | None, timeout: float = 10.0) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def createIsolatedSimulation(
    run_dir: Path,
    http_port: int,
    cfg: dict,
) -> tuple[Path, int]:
    """Create an isolated copy of simulation.as implementing fault_code 9."""
    scenario = cfg.get("scenario", {})
    normal_days = scenario.get("normal_days", 30)
    fault_day = scenario.get("fault_day", 31)
    fault_injection_tick = scenario.get("fault_injection_tick", normal_days * 24 + 3)
    flare_ramp_rate = scenario.get("flare_ramp_rate", 0.15)
    feed_ramp_rate = scenario.get("feed_ramp_rate", 1.15)
    tick_sleep_ms = scenario.get("tick_sleep_ms", 10)
    max_iterations = fault_day * 24

    sim_copy = run_dir / "s7shell_simulation"
    sim_copy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SIM_DIR / "schema.scl", sim_copy / "schema.scl")

    base_source = (SIM_DIR / "simulation.as").read_text(encoding="utf-8")

    # Point WebSocket to the isolated gateway port
    base_source = base_source.replace(
        'const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";',
        f'const string GATEWAY_WS_URL = "ws://127.0.0.1:{http_port}/ws";',
        1,
    )

    # Inject fault_code 9 constants and state tracking
    fault_injection_code = f"""
// ─── Scenario Constants (Isolated Fault 9: FLARE_BLOCKED) ───
const int NORMAL_DAYS = {normal_days};
const int FAULT_DAY = {fault_day};
const int FAULT_INJECTION_TICK = {fault_injection_tick}; // Day {fault_day}, Hour 3
const double FLARE_RAMP_RATE = {flare_ramp_rate};
const double FEED_RAMP_RATE = {feed_ramp_rate};
const int MAX_SIM_ITERATIONS = {max_iterations};
const int TICK_SLEEP_MS = {tick_sleep_ms};
int esd_trip_tick = -1;
"""

    main_marker = "void main() {"
    if main_marker not in base_source:
        raise RuntimeError("Could not find 'void main() {' marker in simulation.as")

    base_source = base_source.replace(main_marker, fault_injection_code + "\n" + main_marker, 1)

    # In simulatePhysics(), implement fault 9 physics
    physics_marker = "void simulatePhysics() {"
    physics_impl = f"""void simulatePhysics() {{
    tick_count++;
    bool fault_active = (tick_count >= FAULT_INJECTION_TICK);

    // ── Tower cycle sequencer ──
    for (int i = 0; i < NUM_TOWERS; i++) {{
        time_in_state[i] += 1.0;

        if (i == lead_lag_index) {{
            if (cycle_state[i] == 1 && time_in_state[i] >= T_DEPRESS) {{
                cycle_state[i] = 2;
                time_in_state[i] = 0.0;
            }} else if (cycle_state[i] == 2 && time_in_state[i] >= T_HEAT) {{
                cycle_state[i] = 3;
                time_in_state[i] = 0.0;
            }} else if (cycle_state[i] == 3 && time_in_state[i] >= T_COOL) {{
                cycle_state[i] = 4;
                time_in_state[i] = 0.0;
            }} else if (cycle_state[i] == 4 && time_in_state[i] >= T_REPRESS) {{
                cycle_state[i] = 0;
                time_in_state[i] = 0.0;
                moisture_pct[i] = 3.0;
                bed_dp[i] = 0.20;
                cycles_done[i]++;

                lead_lag_index = (lead_lag_index + 1) % NUM_TOWERS;
                cycle_state[lead_lag_index] = 1;
                time_in_state[lead_lag_index] = 0.0;
            }}
        }} else {{
            moisture_pct[i] += 0.05 + (feed_flow > 850.0 ? 0.02 : 0.0);
            if (moisture_pct[i] > 95.0) moisture_pct[i] = 95.0;
            bed_dp[i] = 0.20 + (moisture_pct[i] / 100.0) * 0.6 + randomNoiseAmp(0.02);
            if (fault_active) {{
                // Small bed_dp rise under relief blockage
                bed_dp[i] += 0.003 * double(tick_count - FAULT_INJECTION_TICK);
            }}
        }}
    }}

    // ── Apply random process noise and fault physics ──
    feed_flow = base_feed_flow + randomNoiseAmp(15.0);
    coalescer_dp += 0.0001;
    if (coalescer_dp > 0.8) coalescer_dp = 0.8;

    if (fault_active) {{
        int elapsed = tick_count - FAULT_INJECTION_TICK;
        // 1. flare_header_press ramps up from 1.05 bar (+0.15 bar/tick)
        flare_header_press = 1.05 + FLARE_RAMP_RATE * double(elapsed) + randomNoiseAmp(0.02);

        // 2. Relief is ineffective: feed_press ramps up
        double ramp = FEED_RAMP_RATE * double(elapsed);
        feed_press = base_feed_press + ramp + randomNoiseAmp(0.5);

        // 3. Lagged rise on downstream pressures
        double target_scrubber = feed_press - 0.1;
        double target_inlet = feed_press - 0.3;
        system_inlet_press += (target_inlet - system_inlet_press) * 0.65;
        double target_outlet = system_inlet_press - 1.5 - (bed_dp[0] + bed_dp[1] + bed_dp[2]) / 3.0;
        system_outlet_press += (target_outlet - system_outlet_press) * 0.65;
    }} else {{
        flare_header_press = 1.05 + randomNoiseAmp(0.01);
        feed_press = base_feed_press + randomNoiseAmp(0.5);
        system_inlet_press = feed_press - 0.3;
        system_outlet_press = system_inlet_press - 1.5 - (bed_dp[0] + bed_dp[1] + bed_dp[2]) / 3.0;
    }}

    // ── Regen gas dynamics ──
    int regen_state = cycle_state[lead_lag_index];
    if (regen_state == 2) {{
        heater_flame_on = true;
        double error = 290.0 - heater_outlet_temp;
        heater_duty = 40.0 + error * 1.5 + randomNoiseAmp(2.0);
        if (heater_duty > 100.0) heater_duty = 100.0;
        if (heater_duty < 0.0) heater_duty = 0.0;
        heater_outlet_temp += error * 0.2 + randomNoiseAmp(1.0);
        blower_running = true;
        blower_flow = 220.0 + randomNoiseAmp(5.0);
    }} else if (regen_state == 3) {{
        heater_flame_on = false;
        heater_duty = 0.0;
        heater_outlet_temp -= (heater_outlet_temp - feed_temp) * 0.2 + randomNoiseAmp(0.5);
        cooler_outlet_temp -= (cooler_outlet_temp - 35.0) * 0.3 + randomNoiseAmp(0.5);
        blower_running = true;
        blower_flow = 220.0 + randomNoiseAmp(5.0);
        regen_water_removed += 0.8;
    }} else {{
        heater_flame_on = false;
        heater_duty = 0.0;
        blower_running = (regen_state == 1 || regen_state == 4);
        blower_flow = blower_running ? (150.0 + randomNoiseAmp(2.0)) : 0.0;
        cooler_outlet_temp += (feed_temp - cooler_outlet_temp) * 0.1;
    }}

    // ── Dew point tracks worst moisture ──
    double worst_moisture = 0.0;
    for (int i = 0; i < NUM_TOWERS; i++) {{
        if (i != lead_lag_index && moisture_pct[i] > worst_moisture) {{
            worst_moisture = moisture_pct[i];
        }}
    }}
    dew_point_c = -115.0 + (worst_moisture / 95.0) * 25.0;

    // ── ESD trip logic ──
    if (feed_press > 90.0 || dew_point_c > DEW_POINT_SPEC + 5.0 || heater_outlet_temp > 320.0) {{
        esd_active = true;
        if (esd_trip_tick < 0) {{
            esd_trip_tick = tick_count;
        }}
    }}
}}
"""
    # Replace simulatePhysics body
    physics_start = base_source.find(physics_marker)
    if physics_start == -1:
        raise RuntimeError("Could not find simulatePhysics() in simulation.as")
    physics_end = base_source.find("void writeInletSeparation", physics_start)
    if physics_end == -1:
        raise RuntimeError("Could not find writeInletSeparation marker in simulation.as")
    base_source = base_source[:physics_start] + physics_impl + "\n" + base_source[physics_end:]

    # Update while loop in main() to run for max_iterations and terminate on ESD
    loop_start_marker = "    int iteration = 0;\n    while (true) {"
    new_loop = f"""    int iteration = 0;
    while (iteration < MAX_SIM_ITERATIONS) {{
        simulatePhysics();
        DTL@ ts = simulatedDTL();
        writeInletSeparation(ts);
        writeAdsorberTowers(ts);
        writeRegenSystem(ts);
        writeSwitchingValves(ts);
        writeOutletQuality(ts);
        writeSafetySystems(ts);
        writeUtilities(ts);

        if (iteration % 24 == 0) {{
            printReport(ts, iteration);
        }}

        if (esd_active) {{
            print("[ESD_EVENT] feed_press=" + feed_press + " bar tripped ESD at tick " + tick_count + " (iteration " + iteration + ")\\n");
            // Give WebSocket time to flush final dirty state
            sleep(100);
            break;
        }}

        iteration++;
        sleep(TICK_SLEEP_MS);
    }}
    print("[SIMULATION_COMPLETE] Finished at iteration " + iteration + " total ticks " + tick_count + "\\n");
    return;
}}
"""
    old_loop_pos = base_source.find(loop_start_marker)
    if old_loop_pos == -1:
        raise RuntimeError("Could not find simulation loop start in simulation.as")
    # Replace until end of main()
    main_end_pos = base_source.find("void simulatePhysics()", old_loop_pos)
    base_source = base_source[:old_loop_pos] + new_loop + "\n" + base_source[main_end_pos:]

    script_path = sim_copy / "simulation.as"
    script_path.write_text(base_source, encoding="utf-8")
    return script_path, max_iterations


def writeGatewayConfig(path: Path, state_dir: Path, http_port: int, schema_path: Path) -> None:
    config = {
        "schema": str(schema_path.resolve()),
        "state_dir": str(state_dir.resolve()),
        "security_policy": "permissive",
        "northbound": {
            "http": {
                "ip": "127.0.0.1",
                "port": http_port,
                "rate_limit_max_requests": 60000,
                "rate_limit_window_s": 60,
            },
            "websocket": {"ip": "127.0.0.1", "port": http_port},
        },
        "persistence": {
            "enabled": True,
            "mode": "changes_with_timestamp",
            "format": "jsonl",
            "atomic_window_ms": 1,
            "batch_interval_s": 3600,
            "anchor_interval_s": 300,
            "cloud_sync": {"enabled": False},
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config, indent=2), encoding="utf-8")


def runCapture(args) -> tuple[Path, Path]:
    cfg = loadConfig(getConfigPath(Path(__file__).parent))
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    run_dir = out_dir / datetime.now(timezone.utc).strftime("capture_%Y%m%d_%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=False)

    gw_bin = findBinary("gateway")
    s7_bin = findBinary("s7shell")

    script_path, max_iterations = createIsolatedSimulation(run_dir, args.port, cfg)
    gw_config_path = run_dir / "gateway.json"
    gw_state_dir = run_dir / "gw_state"
    writeGatewayConfig(gw_config_path, gw_state_dir, args.port, script_path.parent / "schema.scl")

    # Discard verbose subprocess output by default to avoid massive log files.
    # Use DEVNULL for stdout/stderr unless --verbose flag is provided.
    if args.verbose:
        gw_log = open(run_dir / "gateway.log", "w", encoding="utf-8")
        sim_log = open(run_dir / "s7shell.log", "w", encoding="utf-8")
        gw_stdout = gw_log
        gw_stderr = subprocess.STDOUT
        sim_stdout = sim_log
        sim_stderr = subprocess.STDOUT
    else:
        gw_log = sim_log = None
        gw_stdout = gw_stderr = subprocess.DEVNULL
        sim_stdout = sim_stderr = subprocess.DEVNULL

    gw_proc = subprocess.Popen(
        [str(gw_bin), str(gw_config_path)],
        cwd=ROOT,
        stdout=gw_stdout,
        stderr=gw_stderr,
    )
    sim_proc = None
    sim_output = ""

    try:
        print(f"[capture] Starting Gateway on http://127.0.0.1:{args.port}...")
        waitHttp(f"http://127.0.0.1:{args.port}/endpoints", gw_proc)
        print("[capture] Gateway ready. Starting S7Shell simulation (30 normal days + fault day)...")

        sim_proc = subprocess.Popen(
            [str(s7_bin), script_path.name],
            cwd=script_path.parent,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        stdout, _ = sim_proc.communicate()
        sim_output = stdout
        if sim_log:
            sim_log.write(stdout)
            sim_log.flush()

        if sim_proc.returncode != 0:
            raise RuntimeError(f"S7Shell exited with code {sim_proc.returncode}; inspect {run_dir / 's7shell.log' if args.verbose else 'run with --verbose to see logs'}")

        print("[capture] Simulation complete.")
    finally:
        stopProcess(sim_proc)
        # Give gateway a moment to finish commit before stopping
        time.sleep(1.0)
        stopProcess(gw_proc, timeout=10)
        if gw_log:
            gw_log.close()
        if sim_log:
            sim_log.close()

    # Locate the generated .jsonl.zst archive
    archives = sorted(gw_state_dir.rglob("*.jsonl.zst"), key=lambda p: p.stat().st_mtime)
    if not archives:
        log_hint = run_dir / "gateway.log" if args.verbose else "run with --verbose to generate gateway.log"
        raise RuntimeError(f"No .jsonl.zst archive found under {gw_state_dir}; check {log_hint}")

    # Copy / move the final archive to a canonical location in out_dir
    raw_archive = archives[-1]
    final_archive = out_dir / "flare_history.jsonl.zst"
    shutil.copy2(raw_archive, final_archive)
    print(f"[capture] Archive generated: {final_archive} ({final_archive.stat().st_size:,} bytes)")

    # Parse ESD and fault onset timestamps
    epoch_anchor = 946684800000  # ms (2000-01-01 00:00:00 UTC)
    ms_per_hour = 3600 * 1000

    fault_tick = cfg.get("scenario", {}).get("fault_injection_tick", 723)
    fault_onset_ts = epoch_anchor + fault_tick * ms_per_hour

    esd_tick = None
    esd_ts = None
    esd_match = re.search(r"tripped ESD at tick (\d+)", sim_output)
    if esd_match:
        esd_tick = int(esd_match.group(1))
        esd_ts = epoch_anchor + esd_tick * ms_per_hour
        print(f"[capture] ESD detected at tick {esd_tick} (Hour {esd_tick % 24} of Day {1 + esd_tick // 24})")
    else:
        print("[capture] Run completed without ESD trip.")

    meta = {
        "fault_code": 9,
        "fault_name": "FLARE_BLOCKED",
        "scenario": cfg.get("scenario", {}),
        "fault_onset_tick": fault_tick,
        "fault_onset_ts": fault_onset_ts,
        "fault_onset_iso": datetime.fromtimestamp(fault_onset_ts / 1000.0, tz=timezone.utc).isoformat(),
        "esd_tick": esd_tick,
        "esd_ts": esd_ts,
        "esd_iso": datetime.fromtimestamp(esd_ts / 1000.0, tz=timezone.utc).isoformat() if esd_ts else None,
        "archive_file": final_archive.name,
        "archive_bytes": final_archive.stat().st_size,
        "generated_at": datetime.now(timezone.utc).isoformat(),
    }

    meta_path = out_dir / "meta.json"
    meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"[capture] Metadata written: {meta_path}")

    return final_archive, meta_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(Path(__file__).parent), help="Output directory for archive and meta.json")
    parser.add_argument("--port", type=int, default=18080, help="Gateway HTTP/WS port for capture")
    parser.add_argument("--verbose", action="store_true", help="Enable verbose subprocess logging to files (default: discard output to avoid huge logs)")
    args = parser.parse_args()

    try:
        runCapture(args)
    except Exception as exc:
        print(f"[capture] FAILED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
