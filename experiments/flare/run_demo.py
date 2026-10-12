#!/usr/bin/env python3
"""Live Demo: Replay archive with sgrn_replay, stream to detector, and print agent advisories.

Pipeline:
  1. Replay flare_history.jsonl.zst with sgrn_replay.
  2. Start prediction gateway beside it.
  3. Detector polls the replayed state, maintains sliding window, and evaluates detect().
  4. LangChain agent prints compact block on state change (normal -> uncertain -> abnormal)
     or every N cycles (routine report), ground strictly in detector state.
  5. Supports --no-llm for fast, reproducible offline evaluation.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Tuple

import numpy as np

# Ensure sgrn python packages are accessible
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "sgrn/python") not in sys.path:
    sys.path.insert(0, str(ROOT / "sgrn/python"))

from sgrn.gateway import Gateway, GatewayError

from agent import FlareMonitoringAgent
from dataset import SCL_SCHEMA
from langchain_tools import GLOBAL_STORE
from model import FlareDetector

FLARE_DIR = Path(__file__).parent
CONFIG_PATH = FLARE_DIR / "config.json"
META_PATH = FLARE_DIR / "meta.json"
ARCHIVE_PATH = FLARE_DIR / "flare_history.jsonl.zst"


def find_binary(name: str) -> Path:
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
    raise FileNotFoundError(f"Binary {name} not found")


def wait_http(url: str, proc: subprocess.Popen, timeout: float = 30.0) -> None:
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


def stop_process(proc: subprocess.Popen | None, timeout: float = 10.0) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def write_gateway_config(path: Path, state_dir: Path, http_port: int, schema: Path) -> None:
    config = {
        "schema": str(schema),
        "state_dir": str(state_dir),
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
        "persistence": {"enabled": False},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config, indent=2), encoding="utf-8")


def set_replay_speed(base_url: str, speed: float) -> bool:
    try:
        req = urllib.request.Request(
            f"{base_url}/replay/speed",
            data=json.dumps({"speed": speed}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=1) as resp:
            return resp.status == 200
    except Exception:
        return False


def extract_features_from_state(gw: Gateway, features: list[str]) -> Tuple[Dict[str, float], str]:
    """Read DBs and extract values for each configured feature."""
    values = {}
    sim_time = "N/A"

    try:
        # Read the relevant DBs
        inlet = gw.readData("InletSeparation")
        adsorber = gw.readData("AdsorberTowers")
        regen = gw.readData("RegenSystem")
        outlet = gw.readData("OutletQuality")
        safety = gw.readData("SafetySystems")
        utils = gw.readData("Utilities")

        state_tree = {
            "InletSeparation": inlet,
            "AdsorberTowers": adsorber,
            "RegenSystem": regen,
            "OutletQuality": outlet,
            "SafetySystems": safety,
            "Utilities": utils,
        }

        # Timestamp
        sim_time = str(inlet.get("timestamp", "N/A"))

        for feat in features:
            parts = feat.split(".")
            curr = state_tree
            val = 0.0
            try:
                for part in parts:
                    if "[" in part and "]" in part:
                        # Array index e.g. towers[0]
                        base_name, idx_str = part[:-1].split("[")
                        curr = curr[base_name][int(idx_str)]
                    else:
                        curr = curr[part]
                val = float(curr)
            except Exception:
                val = 0.0
            values[feat] = val

    except Exception as exc:
        raise GatewayError(f"Failed to read plant state: {exc}")

    return values, sim_time


def run_live_demo(args) -> int:
    config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    meta = json.loads(META_PATH.read_text(encoding="utf-8")) if META_PATH.is_file() else {}

    run_dir = FLARE_DIR / f"demo_{datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')}"
    run_dir.mkdir(parents=True, exist_ok=True)

    archive = Path(args.archive).resolve() if args.archive else ARCHIVE_PATH
    if not archive.is_file():
        raise FileNotFoundError(f"Archive not found: {archive}; run run_capture.py first.")

    replay_bin = find_binary("sgrn_replay")
    gateway_bin = find_binary("gateway")

    # Load trained detector
    detector = FlareDetector.load(FLARE_DIR, config)
    features = detector.features
    window_size = detector.window_size

    # Setup configurations
    cfg_a = run_dir / "replay_gw.json"
    cfg_b = run_dir / "pred_gw.json"
    write_gateway_config(cfg_a, run_dir / "replay_state", args.port_a, SCL_SCHEMA)
    write_gateway_config(cfg_b, run_dir / "pred_state", args.port_b, SCL_SCHEMA)

    # Discard verbose subprocess output by default to avoid massive log files.
    # Use DEVNULL for stdout/stderr unless --verbose flag is provided.
    if args.verbose:
        log_a = open(run_dir / "replay.log", "w", encoding="utf-8")
        log_b = open(run_dir / "prediction.log", "w", encoding="utf-8")
        stdout_a = log_a
        stdout_b = log_b
        stderr_a = subprocess.STDOUT
        stderr_b = subprocess.STDOUT
    else:
        log_a = log_b = None
        stdout_a = stdout_b = subprocess.DEVNULL
        stderr_a = stderr_b = subprocess.DEVNULL

    replay_proc = subprocess.Popen(
        [
            str(replay_bin),
            "--config", str(cfg_a),
            "--archive", str(archive),
            "--schema", str(SCL_SCHEMA),
            "--speed", str(args.speed),
            "--loop",
        ],
        cwd=ROOT,
        stdout=stdout_a,
        stderr=stderr_a,
    )
    pred_proc = subprocess.Popen(
        [str(gateway_bin), str(cfg_b)],
        cwd=ROOT,
        stdout=stdout_b,
        stderr=stderr_b,
    )

    url_a = f"http://127.0.0.1:{args.port_a}"
    url_b = f"http://127.0.0.1:{args.port_b}"

    agent = FlareMonitoringAgent(use_llm=not args.no_llm)

    print("\n" + "=" * 80)
    print("  SGRN Gas Processing — Flare Blocked Demonstration")
    print("================================================================================")
    print(f"  Replay Gateway: {url_a}")
    print(f"  Prediction Gateway: {url_b}")
    print(f"  Archive: {archive.name} | Speed: {args.speed}x")
    print(f"  Detector: Autoencoder with Split-Conformal Bounds (alpha={detector.alpha})")
    print(f"  Thresholds: q_alpha = {detector.q_alpha:.3f}, q_upper = {detector.q_upper:.3f}")
    print("================================================================================\n")

    sliding_window = []
    previous_state = "normal"
    cycle_counter = 0
    poll_count = 0

    try:
        wait_http(f"{url_a}/endpoints", replay_proc)
        wait_http(f"{url_b}/endpoints", pred_proc)
        print("[demo] Replay pipeline active. Streaming telemetry to detector and Prediction Gateway...\n")

        gw_a = Gateway(url_a)
        gw_b = Gateway(url_b)
        last_timestamp = ""
        b_pushed_once = False

        while True:
            time.sleep(args.poll_interval)
            try:
                feature_dict, sim_time = extract_features_from_state(gw_a, features)
            except Exception:
                continue

            if sim_time == last_timestamp or sim_time == "N/A":
                continue
            last_timestamp = sim_time
            poll_count += 1

            # Append to sliding window
            vec = [feature_dict[f] for f in features]
            sliding_window.append(vec)
            if len(sliding_window) > window_size:
                sliding_window.pop(0)

            if len(sliding_window) == window_size:
                win_arr = np.array(sliding_window, dtype=np.float32)
                det_result = detector.detect(win_arr)
                det_result["sim_time"] = sim_time
                curr_state = det_result["state"]

                # ── Stream Predictions and Mirrored State to Prediction Gateway B ──
                is_abnormal = bool(curr_state == "abnormal")
                feed_p = feature_dict.get("InletSeparation.feed_pressure", 68.0)
                trip_advice = bool(is_abnormal and feed_p > 85.0)
                fault_code = 9 if is_abnormal else 0

                try:
                    gw_b.writeFields("InletSeparation", {
                        "feed_pressure": feed_p,
                        "feed_temp": feature_dict.get("InletSeparation.feed_temp", 32.0),
                        "coalescer_dp": feature_dict.get("InletSeparation.coalescer_dp", 0.15),
                        "scrubber_press": feature_dict.get("InletSeparation.scrubber_press", 67.9),
                    })
                    gw_b.writeFields("RegenSystem", {
                        "heater_outlet_temp": feature_dict.get("RegenSystem.heater_outlet_temp", 35.0),
                        "cooler_inlet_temp": feature_dict.get("RegenSystem.cooler_inlet_temp", 35.0),
                        "cooler_outlet_temp": feature_dict.get("RegenSystem.cooler_outlet_temp", 35.0),
                    })
                    gw_b.writeFields("SafetySystems", {
                        "flare_header_press": feature_dict.get("SafetySystems.flare_header_press", 1.05),
                        "model_anomaly": is_abnormal,
                        "model_fault_code": int(fault_code),
                        "model_trip_advice": trip_advice,
                    })
                    gw_b.writeFields("OutletQuality", {
                        "dew_point": feature_dict.get("OutletQuality.dew_point", -105.0),
                        "sales_gas_press": feature_dict.get("OutletQuality.sales_gas_press", 65.0),
                        "dew_point_forecast": float(feature_dict.get("OutletQuality.dew_point", -105.0)),
                        "dew_point_forecast_std": float(det_result.get("score", 0.0)),
                    })
                    gw_b.writeFields("Utilities", {
                        "fuel_gas_press": feature_dict.get("Utilities.fuel_gas_press", 3.2),
                        "inst_air_press": feature_dict.get("Utilities.inst_air_press", 7.0),
                        "regen_comp_disch_press": feature_dict.get("Utilities.regen_comp_disch_press", 68.0),
                    })
                    if not b_pushed_once:
                        print(f"[demo] Prediction Gateway ({url_b}) rendering live model predictions and plant mirror.")
                        b_pushed_once = True
                except Exception as push_exc:
                    pass

                # Update global tool store
                replay_info = {
                    "sim_time": sim_time,
                    "speed": args.speed,
                    "frames": poll_count,
                    "running": True,
                }
                GLOBAL_STORE.update(det_result, sim_time, feature_dict, replay_info)

                # State change trigger or periodic report
                state_changed = (curr_state != previous_state)
                cycle_counter += 1
                periodic_trigger = (cycle_counter % args.report_cycles == 0)

                if state_changed or periodic_trigger:
                    print(f"[trigger] {'STATE TRANSITION: ' + previous_state.upper() + ' -> ' + curr_state.upper() if state_changed else 'ROUTINE REPORT (every ' + str(args.report_cycles) + ' cycles)'}")
                    agent.print_operator_block(det_result)
                    previous_state = curr_state

                # Check for ESD termination
                if feed_p >= 90.0:
                    print(f"\n[ESD_TRIP] feed_pressure={feed_p:.2f} bar exceeded 90 bar limit at {sim_time}.")
                    print("[ESD_TRIP] Plant Emergency Shutdown initiated. Replay run concluded.\n")
                    break

    except KeyboardInterrupt:
        print("\n[demo] Demo interrupted by user.")
    finally:
        stop_process(replay_proc)
        stop_process(pred_proc)
        if log_a:
            log_a.close()
        if log_b:
            log_b.close()
        print(f"\n[demo] Demo finished. Run directory: {run_dir}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", help="Path to .jsonl.zst archive (default: flare_history.jsonl.zst)")
    parser.add_argument("--speed", type=float, default=60.0, help="Replay speed multiplier (default: 60x)")
    parser.add_argument("--port-a", type=int, default=18080, help="Replay gateway HTTP port")
    parser.add_argument("--port-b", type=int, default=18082, help="Prediction gateway HTTP port")
    parser.add_argument("--poll-interval", type=float, default=0.04, help="Polling interval in seconds")
    parser.add_argument("--report-cycles", type=int, default=24, help="Periodic advisory interval in cycles")
    parser.add_argument("--no-llm", action="store_true", help="Run offline using verified template without calling external LLM API")
    parser.add_argument("--verbose", action="store_true", help="Enable verbose subprocess logging to files (default: discard output to avoid huge logs)")
    args = parser.parse_args()

    return run_live_demo(args)


if __name__ == "__main__":
    raise SystemExit(main())
