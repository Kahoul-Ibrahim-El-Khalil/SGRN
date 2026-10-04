#!/usr/bin/env python3
"""Record an S7Shell plant run, replay it as a gateway, and predict beside it."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

from gas_processing import (
    ACTIONS,
    FEATURE_NAMES,
    H,
    NOMINAL,
    TWIN_SCHEMA,
    TWIN_SUBS_A,
    TwinStream,
    bindings,
    checker,
    doerPolicy,
    loadOrTrainBundle,
    openSideBySide,
    predictWithUq,
    twinFeatures,
)

ROOT = Path(__file__).resolve().parents[1]
SIM_DIR = ROOT / "sgrn/lib/gateway/simulations/gas_processing"
PREDICTION_SCHEMA = Path(__file__).with_name("history_twin_prediction.scl")
FAULT_IDS = {
    "none": 0,
    "heater-flameout": 1,
    "pressure-surge": 2,
    "feed-overload": 3,
    "blower-failure": 4,
    "analyzer-bias": 5,
}


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
    raise FileNotFoundError(f"Cannot find {name}; build the Linux gateway tools first.")


def waitHttp(url: str, proc: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"process exited early with status {proc.returncode}")
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


def createRecordingSimulation(run_dir: Path, fault: str, fault_tick: int,
                                s7_port: int, duration: int, archive_dir: Path) -> Path:
    """Make an isolated, configured copy of the S7Shell gas plant script."""
    sim_copy = run_dir / "s7shell_simulation"
    sim_copy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SIM_DIR / "schema.scl", sim_copy / "schema.scl")
    source = (SIM_DIR / "simulation.as").read_text(encoding="utf-8")

    source = source.replace(
        "@plc = S7Client(ip, rack, slot);",
        f"@plc = S7Client(ip, rack, slot, {s7_port});",
        1,
    )
    source = source.replace(
        "@opc = OpcUaServer(plc.runtime(), 4840);",
        "@opc = OpcUaServer(plc.runtime(), 14840);",
        1,
    )
    if "OpcUaServer(plc.runtime(), 14840)" not in source:
        raise RuntimeError("Could not assign the isolated OPC-UA port in simulation.as")
    main_marker = next((line for line in source.splitlines() if "Main Simulation Loop" in line), None)
    if main_marker is None:
        raise RuntimeError("Could not locate the main loop marker in simulation.as")
    source = source.replace(
        main_marker,
        f"""const int SGRN_DEMO_FAULT_ID = {FAULT_IDS[fault]};
const int SGRN_DEMO_FAULT_TICK = {fault_tick};

void applyDemoFault(int iteration) {{
    if (SGRN_DEMO_FAULT_ID == 0 || iteration < SGRN_DEMO_FAULT_TICK) return;
    if (SGRN_DEMO_FAULT_ID == 1) {{ // heater flame-out
        heater_flame_on = false;
        heater_duty = 0.0;
        if (heater_outlet_temp > feed_temp) heater_outlet_temp -= 2.0;
    }} else if (SGRN_DEMO_FAULT_ID == 2) {{ // feed pressure surge
        feed_press = 94.0;
        system_inlet_press = 93.7;
        system_outlet_press = 91.0;
        esd_active = true;
    }} else if (SGRN_DEMO_FAULT_ID == 3) {{ // feed overload
        feed_flow = 1050.0;
        coalescer_dp += 0.01;
    }} else if (SGRN_DEMO_FAULT_ID == 4) {{ // blower failure
        blower_running = false;
        blower_flow = 0.0;
    }} else if (SGRN_DEMO_FAULT_ID == 5) {{ // analyzer bias
        dew_point_c += 14.0;
        if (dew_point_c > DEW_POINT_SPEC + 5.0) esd_active = true;
    }}
}}

{main_marker}""",
        1,
    )
    source = source.replace(
        "        simulatePhysics();",
        "        simulatePhysics();\n        applyDemoFault(iteration);",
        1,
    )
    loop_marker = "    int iteration = 0;\n    while (true) {"
    loop_replacement = f'''    string archive_dir = "{archive_dir.as_posix()}";
    Persistence@ persistence = Persistence(plc.runtime(), archive_dir);
    persistence.configure(archive_dir, "binary", "changes_with_timestamp");
    persistence.start();
    print("[S7Shell] recording WAL to " + persistence.outDir());

    int iteration = 0;
    while (iteration < {duration}) {{'''
    if loop_marker not in source:
        raise RuntimeError("Could not locate the simulation loop for persistence setup")
    source = source.replace(loop_marker, loop_replacement, 1)
    loop_end = "        iteration++;\n        sleep(1000);\n    }"
    if loop_end not in source:
        raise RuntimeError("Could not locate the simulation loop end for WAL finalization")
    source = source.replace(
        loop_end,
        "        iteration++;\n        sleep(1000);\n    }\n\n    persistence.flush();\n    persistence.stop();\n    print(\"[S7Shell] finalized WAL: \" + persistence.outDir());",
        1,
    )
    if "SGRN_DEMO_FAULT_ID" not in source or f"S7Client(ip, rack, slot, {s7_port})" not in source:
        raise RuntimeError("Could not apply the selected fault and port settings to simulation.as")
    script = sim_copy / "simulation.as"
    script.write_text(source, encoding="utf-8")
    return script


def writeGatewayConfig(path: Path, state_dir: Path, http_port: int,
                       s7_port: int | None = None, schema: Path = TWIN_SCHEMA) -> None:
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
    if s7_port is not None:
        config["southbound"] = {"s7": {"ip": "127.0.0.1", "port": s7_port}}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config, indent=2), encoding="utf-8")


def recordHistory(args, run_dir: Path, gateway_bin: Path, s7shell_bin: Path) -> Path:
    record_state = run_dir / "record_state"
    archive_dir = run_dir / "s7shell_history"
    config = run_dir / "record_gateway.json"
    writeGatewayConfig(config, record_state, args.record_http_port, args.s7_port)
    script = createRecordingSimulation(
        run_dir, args.fault, args.fault_tick, args.s7_port,
        max(1, int(args.duration)), archive_dir,
    )
    gateway_log = open(run_dir / "record_gateway.log", "w", encoding="utf-8")
    sim_log = open(run_dir / "record_s7shell.log", "w", encoding="utf-8")
    gateway = subprocess.Popen([str(gateway_bin), str(config)], cwd=ROOT,
                               stdout=gateway_log, stderr=subprocess.STDOUT)
    shell = None
    try:
        waitHttp(f"http://127.0.0.1:{args.record_http_port}/endpoints", gateway)
        shell = subprocess.Popen([str(s7shell_bin), script.name], cwd=script.parent,
                                 stdout=sim_log, stderr=subprocess.STDOUT)
        print(f"[record] S7Shell recording for {args.duration}s; fault={args.fault} at tick {args.fault_tick}")
        while shell.poll() is None:
            if gateway.poll() is not None:
                raise RuntimeError(f"recording gateway exited with status {gateway.returncode}")
            time.sleep(0.25)
        if shell.returncode != 0:
            raise RuntimeError(f"S7Shell exited with status {shell.returncode}; see {run_dir / 'record_s7shell.log'}")
    finally:
        stopProcess(shell)
        stopProcess(gateway, timeout=15)
        gateway_log.close()
        sim_log.close()

    archives = sorted(archive_dir.rglob("*.bin.zst"), key=lambda p: p.stat().st_mtime)
    if not archives:
        raise RuntimeError(f"No finalized binary history archive found under {archive_dir}; "
                            f"inspect record_s7shell.log")
    archive = archives[-1]
    print(f"[record] history written: {archive}")
    return archive


def fallbackRow() -> dict:
    row = dict(NOMINAL)
    row["AdsorberTowers.lead_lag_index"] = 0
    row["AdsorberTowers.regen_state"] = 0
    for i in range(3):
        row[f"cycle_{i}"] = 0
    return row


async def predictReplay(url_a: str, url_b: str, bundle, replay: subprocess.Popen) -> None:
    stream = TwinStream(url_a)
    await stream.start(TWIN_SUBS_A)
    row = fallbackRow()
    previous_flush = stream.flushes
    updates = fallback_reads = 0
    try:
        while replay.poll() is None:
            await asyncio.sleep(0.04)
            if stream.flushes == previous_flush:
                continue
            previous_flush = stream.flushes
            X, missing = twinFeatures(stream, row)
            fallback_reads += missing
            updates += 1
            output = predictWithUq(bundle, X)
            prediction = float(output["dew_mean"][0])
            uncertainty = float(output["dew_std"][0])
            fault = int(output["fault"][0])
            anomaly = bool(output["recon"][0] > bundle.det_threshold)
            heat = float(stream.value("RegenSystem-heater_outlet_temp", row.get("RegenSystem.heater_outlet_temp", 35.0)))
            pressure = float(stream.value("InletSeparation-feed_pressure", row.get("InletSeparation.feed_pressure", 68.0)))
            moisture = float(X[0][FEATURE_NAMES.index("AdsorberTowers.worst_moisture")])
            action = doerPolicy(fault, prediction, heat, pressure)
            allowed, reason = checker(action, prediction, uncertainty, heat, pressure,
                                      float(output["conf"][0]), moisture)
            trip = action == 3 and allowed
            try:
                Gateway, _, _, _, _ = bindings()
                Gateway(url_b).writeField(
                    "ModelOutput/dew_point_prediction", prediction
                )
                if updates == 1:
                    echoed = Gateway(url_b).readData("ModelOutput/dew_point_prediction")
                    if abs(float(echoed) - prediction) > 1e-4:
                        raise RuntimeError(
                            f"HTTP readback mismatch: wrote {prediction}, read {echoed}"
                        )
                    print("[prediction] HTTP writes confirmed and read back: "
                          f"ModelOutput/dew_point_prediction={float(echoed):.3f} °C")
            except Exception as exc:
                raise RuntimeError(
                    f"prediction update {updates} could not write to {url_b}: {exc}"
                ) from exc

            if updates % 5 == 0:
                dew = float(stream.value("OutletQuality-dew_point", 0.0))
                flag = "FAULT" if anomaly else "      "
                print(f"[twin] {flag} dew={dew:7.1f}C | forecast +{H}={prediction:7.1f}±{uncertainty:4.1f}C | "
                      f"F{fault} {ACTIONS[action]} -> {'ALLOW' if allowed else 'BLOCK'} ({reason}); "
                      "HTTP prediction write confirmed")
    finally:
        await stream.stop()
    print(f"[twin] consumed {updates} replay updates; {fallback_reads} fallback feature reads")


def run(args) -> None:
    run_dir = Path(args.out) / datetime.now().strftime("run_%Y%m%d_%H%M%S_%f")
    run_dir.mkdir(parents=True, exist_ok=False)
    gateway_bin = findBinary("gateway")
    s7shell_bin = findBinary("s7shell")
    replay_bin = findBinary("sgrn_replay")

    archive = Path(args.archive).resolve() if args.archive else recordHistory(args, run_dir, gateway_bin, s7shell_bin)
    if not archive.is_file():
        raise FileNotFoundError(f"History archive not found: {archive}")

    # Reuse the gas demo's trained forecaster and fault classifier.
    args.out = str(Path(args.out).resolve())
    bundle = loadOrTrainBundle(args, Path(args.out))
    url_a = f"http://127.0.0.1:{args.port_a}"
    url_b = f"http://127.0.0.1:{args.port_b}"
    cfg_a = run_dir / "replay_gateway.json"
    cfg_b = run_dir / "prediction_gateway.json"
    writeGatewayConfig(cfg_a, run_dir / "replay_state", args.port_a)
    writeGatewayConfig(cfg_b, run_dir / "prediction_state", args.port_b,
                       schema=PREDICTION_SCHEMA)
    replay_log = open(run_dir / "replay.log", "w", encoding="utf-8")
    prediction_log = open(run_dir / "prediction_gateway.log", "w", encoding="utf-8")
    replay = None
    prediction = None
    browsers = []
    try:
        replay = subprocess.Popen(
            [str(replay_bin), "--config", str(cfg_a), "--archive", str(archive),
             "--schema", str(TWIN_SCHEMA), "--speed", str(args.speed), "--loop"],
            cwd=ROOT, stdout=replay_log, stderr=subprocess.STDOUT,
        )
        prediction = subprocess.Popen([str(gateway_bin), str(cfg_b)], cwd=ROOT,
                                      stdout=prediction_log, stderr=subprocess.STDOUT)
        waitHttp(f"{url_a}/endpoints", replay)
        waitHttp(f"{url_b}/endpoints", prediction)
        if not args.no_browser:
            browsers = openSideBySide(url_a, url_b)
        print(f"[replay] LEFT: recorded S7Shell history at {url_a}")
        print(f"[prediction] RIGHT: live model output at {url_b}")
        print(f"[archive] {archive}")
        asyncio.run(predictReplay(url_a, url_b, bundle, replay))
    except KeyboardInterrupt:
        pass
    finally:
        if not args.keep_up:
            stopProcess(replay)
            stopProcess(prediction)
            for proc in browsers:
                if proc.poll() is None:
                    proc.terminate()
        replay_log.close()
        prediction_log.close()
        print(f"[done] experiment files: {run_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(ROOT / "scratch/gas_history_twin"))
    parser.add_argument("--archive", help="reuse an existing .bin.zst archive and skip S7Shell recording")
    parser.add_argument("--duration", type=float, default=75.0, help="S7Shell recording duration in seconds")
    parser.add_argument("--fault", choices=FAULT_IDS, default="heater-flameout")
    parser.add_argument("--fault-tick", type=int, default=12)
    parser.add_argument("--speed", type=float, default=1.0, help="history replay speed multiplier")
    parser.add_argument("--port-a", type=int, default=18080, help="replayed-history gateway HTTP port")
    parser.add_argument("--port-b", type=int, default=18082, help="prediction gateway HTTP port")
    parser.add_argument("--record-http-port", type=int, default=18084)
    parser.add_argument("--s7-port", type=int, default=1102)
    parser.add_argument("--runs", type=int, default=6, help="training runs per fault for a new model cache")
    parser.add_argument("--seed", type=int, default=11)
    parser.add_argument("--retrain", action="store_true")
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--keep-up", action="store_true")
    args = parser.parse_args()
    if args.duration <= 0 or args.speed <= 0 or args.fault_tick < 0 or args.fault_tick >= args.duration:
        parser.error("duration and speed must be positive; fault-tick must fall inside the recording duration")
    if len({args.port_a, args.port_b, args.record_http_port, args.s7_port, 14840}) != 5:
        parser.error("record, replay, prediction, S7, and OPC-UA ports must be distinct")
    try:
        run(args)
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f"Experiment failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
