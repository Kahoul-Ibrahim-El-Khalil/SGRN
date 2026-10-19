#!/usr/bin/env python3
"""experiments/flare/runCapture.py — Stage 1: WAL capture for FLARE_BLOCKED scenario.

Runs the stock gas_processing simulation directly — the simulation itself now
owns the FLARE_BLOCKED fault physics (fault_code 9) and all scenario runtime
knobs (SGRN_* environment variables: tick grid, fault onset, ramp rates,
termination, pacing). No source rewriting: schema.scl + simulation.as are
copied verbatim into an isolated capture dir and scripted via env.

Two telemetry egress paths:

  * default  — deployment-faithful: s7shell publishes via GatewaySync →
    gateway process → hydrated twin → PersistenceService → WAL (jsonl deltas,
    one merged line per tick; the format sgrn_dataset/dataset.py expect).
  * --local  — in-shell capture: s7shell persists the WAL itself through the
    Persistence binding with full-DB binary images (atomic_window not needed;
    no hydration of the gateway twin is available in a bare-script process,
    so the per-leaf JSONL mode is not usable offline).

Produces:
  <out>/flare_history.jsonl.zst   (WAL archive)
  <out>/meta.json                 (fault/ESD timestamps for evaluation)
"""

from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
SIM_DIR = ROOT / "sgrn/lib/gateway/simulations/gas_processing"

from config_loader import loadConfig, getConfigPath


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


def stageIsolatedSimulation(run_dir: Path, cfg: dict, noFault: bool = False) -> tuple[Path, int, dict]:
    """Stage the stock simulation verbatim; the scenario flows via env vars.

    ``noFault`` forces SGRN_FAULT_CODE=0 (pure nominal run): used to capture a
    second, independent normal run for held-out FAR evaluation
    (`python3 experiments/flare capture --no-fault`). NOTE: the simulator
    seeds its noise LCG with a fixed start state and exposes no seed
    override, so the run is deterministic — see dataset.heldout_source.
    """
    scenario = cfg.get("scenario", {})
    normal_days = scenario.get("normal_days", 30)
    fault_day = scenario.get("fault_day", 31)
    ticks_per_day = int(scenario.get("ticks_per_day", 24))
    if ticks_per_day <= 0:
        raise ValueError(f"scenario.ticks_per_day must be > 0, got {ticks_per_day}")
    tick_sleep_ms = scenario.get("tick_sleep_ms", 10)

    sim_copy = run_dir / "s7shell_simulation"
    sim_copy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SIM_DIR / "schema.scl", sim_copy / "schema.scl")
    shutil.copy2(SIM_DIR / "simulation.as", sim_copy / "simulation.as")

    env = {
        "SGRN_TICKS_PER_DAY": str(ticks_per_day),
        "SGRN_TICK_SLEEP_MS": str(tick_sleep_ms),
        "SGRN_SIM_MAX_ITERATIONS": str(fault_day * ticks_per_day),
        "SGRN_REPORT_EVERY": str(ticks_per_day),
        "SGRN_FAULT_CODE": "0" if noFault else str(scenario.get("fault_code", 9)),
        "SGRN_FAULT_ONSET_HOUR": str(scenario.get("fault_injection_tick", normal_days * 24 + 3)),
        "SGRN_FLARE_RAMP_RATE": repr(scenario.get("flare_ramp_rate", 0.15)),
        "SGRN_FEED_RAMP_RATE": repr(scenario.get("feed_ramp_rate", 1.15)),
        # Keep the sync publish debounce at/below tick pacing so fast-tick
        # captures keep per-tick WAL samples (default throttle is 25 ms).
        "SGRN_SYNC_COALESCE_MS": str(min(int(tick_sleep_ms), 5)),
    }
    return sim_copy, fault_day * ticks_per_day, env


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


def findFirstTickTs(archive_path: Path) -> int | None:
    """Timestamp of the first data frame (WAL ms domain), streamed.

    The simulation clocks DTLs as absolute wall datetimes (2000-01-01
    00:00 + ticks) while frame stamps encode the machine's local mktime
    reading of that wall clock, so meta anchors must come from the archive
    itself rather than a fixed UTC epoch. Handles both merged formats,
    raw or zstd-compressed.
    """
    try:
        import zstandard
    except ImportError:
        return None
    k_control_db = 0xFFFF  # control frames carry wall-clock bookkeeping only

    def _first_tick_from_frames(read, prefix=b""):
        # stream reads may return short chunks — frame alignment demands
        # an exact-length reader
        def exact(n):
            buf = b""
            while len(buf) < n:
                chunk = read(n - len(buf))
                if not chunk:
                    break
                buf += chunk
            return buf

        head = prefix + exact(10 - len(prefix))
        if len(head) < 10 or head[:4] != b"SGRN":
            return None
        exact(struct.unpack_from("<I", head, 6)[0])  # embedded schema blob
        while True:
            hdr = exact(14)
            if len(hdr) < 14:
                return None
            ts = struct.unpack_from("<q", hdr, 0)[0]
            db = struct.unpack_from("<H", hdr, 8)[0]
            if db != k_control_db:
                return ts
            exact(struct.unpack_from("<I", hdr, 10)[0])

    def _first_tick_jsonl(read, prefix=b""):
        pending = prefix
        while True:
            chunk = read(262144)
            if not chunk:
                return None
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if b'"ts"' not in line:
                    continue
                try:
                    rec = json.loads(line.decode("utf-8", errors="replace"))
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
                if rec.get("type") in ("anchor", "delta") and isinstance(rec.get("ts"), (int, float)):
                    return int(rec["ts"])

    try:
        def _exact(read, n):
            buf = b""
            while len(buf) < n:
                chunk = read(n - len(buf))
                if not chunk:
                    break
                buf += chunk
            return buf

        with open(archive_path, "rb") as fh:
            if _exact(fh.read, 4) == b"\x28\xb5\x2f\xfd":  # zstd archive
                fh.seek(0)
                with zstandard.ZstdDecompressor().stream_reader(fh, read_across_frames=True) as zr:
                    head = zr.read(4)
                    if head == b"SGRN":
                        return _first_tick_from_frames(zr.read, head)
                    return _first_tick_jsonl(zr.read, head)
            fh.seek(0)
            if _exact(fh.read, 4) == b"SGRN":
                fh.seek(0)
                return _first_tick_from_frames(fh.read)
            fh.seek(0)
            return _first_tick_jsonl(fh.read)
    except OSError:
        return None

def parseEsdTick(sim_output: str) -> int | None:
    match = re.search(r"tripped ESD at tick (\d+)", sim_output)
    return int(match.group(1)) if match else None


def runCaptureGateway(args, cfg: dict, run_dir: Path, out_dir: Path, ticks_per_day: int,
                      noFault: bool = False) -> tuple[Path, str]:
    """Deployment-faithful capture: gateway + GatewaySync (jsonl deltas)."""
    gw_bin = findBinary("gateway")
    s7_bin = findBinary("s7shell")

    script_dir, max_iterations, env = stageIsolatedSimulation(run_dir, cfg, noFault=noFault)
    gw_config_path = run_dir / "gateway.json"
    gw_state_dir = run_dir / "gw_state"
    writeGatewayConfig(gw_config_path, gw_state_dir, args.port, script_dir / "schema.scl")
    env["SGRN_GATEWAY_WS_URL"] = f"ws://127.0.0.1:{args.port}/ws"

    if args.verbose:
        gw_log = open(run_dir / "gateway.log", "w", encoding="utf-8")
        sim_log = open(run_dir / "s7shell.log", "w", encoding="utf-8")
        gw_stdout = gw_log
        sim_stdout = sim_log
    else:
        gw_log = sim_log = None
        gw_stdout = subprocess.DEVNULL
        sim_stdout = subprocess.PIPE

    gw_proc = subprocess.Popen(
        [str(gw_bin), str(gw_config_path)],
        cwd=str(ROOT),
        stdout=gw_stdout,
        stderr=subprocess.STDOUT,
    )
    sim_proc = None
    sim_output = ""
    try:
        print(f"[capture] Gateway starting on http://127.0.0.1:{args.port}...")
        waitHttp(f"http://127.0.0.1:{args.port}/endpoints", gw_proc)
        print(f"[capture] Gateway ready. Starting s7shell simulation — "
              f"{max_iterations} ticks ({ticks_per_day}/day → {24.0 * 60.0 / ticks_per_day:.1f} min/tick)...")

        sim_proc = subprocess.Popen(
            [str(s7_bin), "simulation.as"],
            cwd=str(script_dir),
            env={**os.environ, **env},
            stdout=sim_stdout,
            stderr=subprocess.STDOUT,
            text=True,
        )
        stdout, _ = sim_proc.communicate()
        sim_output = stdout or ""
        if sim_log:
            sim_log.write(sim_output)
            sim_log.flush()
        if sim_proc.returncode != 0:
            hint = run_dir / "s7shell.log" if args.verbose else "run with --verbose to generate logs"
            raise RuntimeError(f"s7shell exited with code {sim_proc.returncode}; inspect {hint}")
        print("[capture] Simulation complete.")
    finally:
        stopProcess(sim_proc)
        time.sleep(1.0)  # let the gateway flush the final deltas
        stopProcess(gw_proc, timeout=10)
        if gw_log:
            gw_log.close()
        if sim_log:
            sim_log.close()

    archives = sorted(gw_state_dir.rglob("*.jsonl.zst"), key=lambda p: p.stat().st_mtime)
    # Recovery: the gateway's async WAL finalizer can leave the archive as a
    # .tmp when shutdown posts the rename to a stopped pool. The .tmp holds a
    # complete WAL (footer included) — promote it for ingestion.
    if not archives:
        for tmp in sorted(gw_state_dir.rglob("*.jsonl.zst.tmp"), key=lambda p: p.stat().st_mtime):
            tmp.rename(tmp.with_name(tmp.name[:-4]))  # strip ".tmp"
        archives = sorted(gw_state_dir.rglob("*.jsonl.zst"), key=lambda p: p.stat().st_mtime)
    if not archives:
        log_hint = run_dir / "gateway.log" if args.verbose else "run with --verbose to generate logs"
        raise RuntimeError(f"No .jsonl.zst archive found under {gw_state_dir}; check {log_hint}")

    final_archive = out_dir / "flare_history.jsonl.zst"
    shutil.copy2(archives[-1], final_archive)

    esd_output = sim_output
    if args.verbose and not sim_output:
        esd_output = (run_dir / "s7shell.log").read_text(encoding="utf-8") if (run_dir / "s7shell.log").exists() else ""
    return final_archive, esd_output


def runCaptureLocal(args, cfg: dict, run_dir: Path, out_dir: Path, ticks_per_day: int,
                    noFault: bool = False) -> tuple[Path, str]:
    """In-shell capture: Persistence binding, full-DB binary images, no gateway.

    sgrn_dataset groups the per-DB frames by timestamp into one complete row
    per tick. The startup WAL anchor (raw, unseeded DB memory) has no
    timestamp column values — downstream dataset.py drops such rows.
    """
    s7_bin = findBinary("s7shell")

    script_dir, max_iterations, env = stageIsolatedSimulation(run_dir, cfg, noFault=noFault)
    capture_dir = run_dir / "wal"
    env["SGRN_CAPTURE_DIR"] = str(capture_dir)

    if args.verbose:
        sim_log = open(run_dir / "s7shell.log", "w", encoding="utf-8")
        sim_stdout = sim_log
    else:
        sim_log = None
        sim_stdout = subprocess.PIPE

    sim_proc = None
    sim_output = ""
    try:
        print(f"[capture] s7shell in-process capture — "
              f"{max_iterations} ticks ({ticks_per_day}/day → {24.0 * 60.0 / ticks_per_day:.1f} min/tick)...")
        sim_proc = subprocess.Popen(
            [str(s7_bin), "simulation.as"],
            cwd=str(script_dir),
            env={**os.environ, **env},
            stdout=sim_stdout,
            stderr=subprocess.STDOUT,
            text=True,
        )
        stdout, _ = sim_proc.communicate()
        sim_output = stdout or ""
        if sim_log:
            sim_log.write(sim_output)
            sim_log.flush()
        if sim_proc.returncode != 0:
            hint = run_dir / "s7shell.log" if args.verbose else "run with --verbose to generate logs"
            raise RuntimeError(f"s7shell exited with code {sim_proc.returncode}; inspect {hint}")
        print("[capture] Simulation complete.")
    finally:
        stopProcess(sim_proc)
        if sim_log:
            sim_log.close()

    archives = sorted(capture_dir.rglob("*.bin.zst"), key=lambda p: p.stat().st_mtime)
    if not archives:
        archives = sorted(capture_dir.rglob("*.jsonl.zst"), key=lambda p: p.stat().st_mtime)
    if not archives:
        log_hint = run_dir / "s7shell.log" if args.verbose else "run with --verbose to generate logs"
        raise RuntimeError(f"No WAL archive found under {capture_dir}; check {log_hint}")

    # Merge the rotated WAL slices into ONE canonical JSONL archive. The
    # transcode emitter now expands struct/primitive arrays to full per-element
    # JSON arrays under the collapsed WAL-dictionary ids, so the merged file
    # decodes with complete element columns (towers[0..2], valves, detectors)
    # and stays the single human-inspectable transfer artifact.
    final_archive = out_dir / "flare_history.jsonl.zst"
    merge_bin = ROOT / ".dist/linux-static-release/sgrn_dataset"
    res = subprocess.run(
        [str(merge_bin), "--merge", str(final_archive), "-f", "jsonl", str(capture_dir)],
        capture_output=True, text=True,
    )
    if res.returncode != 0 or not final_archive.is_file():
        raise RuntimeError(f"WAL merge failed:\n{res.stdout}\n{res.stderr}")
    return final_archive, sim_output


def runCapture(args) -> tuple[Path, Path]:
    if args.config:
        cfg = loadConfig(Path(args.config))
    else:
        cfg = loadConfig(getConfigPath(Path(__file__).parent))
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    noFault = bool(getattr(args, "no_fault", False))
    archive_name = getattr(args, "archive_name", None) or (
        "flare_history_normal.jsonl.zst" if noFault else "flare_history.jsonl.zst")

    run_dir = out_dir / datetime.now(timezone.utc).strftime("capture_%Y%m%d_%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=False)

    scenario = cfg.get("scenario", {})
    ticks_per_day = int(scenario.get("ticks_per_day", 24))
    ms_per_tick = (24 * 3600 * 1000) / ticks_per_day
    period_scale = ticks_per_day / 24.0
    epoch_anchor = 946684800000  # ms (2000-01-01 00:00:00 UTC wall)

    if args.local:
        final_archive, sim_output = runCaptureLocal(args, cfg, run_dir, out_dir, ticks_per_day,
                                                    noFault=noFault)
    else:
        final_archive, sim_output = runCaptureGateway(args, cfg, run_dir, out_dir, ticks_per_day,
                                                      noFault=noFault)
    if final_archive.name != archive_name:
        target = out_dir / archive_name
        final_archive.rename(target)
        final_archive = target
    print(f"[capture] Archive generated: {final_archive} ({final_archive.stat().st_size:,} bytes)")

    # Informational: frame stamps encode the machine-local mktime reading of
    # the simulated wall clock ("2000-01-01 00:00 + ticks"); the modeling clock
    # itself comes from the datablocks' DTLs (see rowCacheTs in sgrn_dataset),
    # which are timezone-free by construction and live in the epoch_anchor
    # domain below. wal_anchor_ms is kept for diagnostics only.
    wal_anchor_ms = findFirstTickTs(final_archive)
    if wal_anchor_ms is None:
        wal_anchor_ms = epoch_anchor
        print("[capture] WARNING: could not read first frame ts; wal_anchor falls back to epoch anchor")
    if wal_anchor_ms != epoch_anchor:
        print(f"[capture] Note: WAL frame anchor {wal_anchor_ms} ms vs DTL epoch anchor {epoch_anchor} ms")

    # Parse ESD/fault onset in SIM ticks; convert to epoch ms via the tick
    # grid. Both ts values share the DTL wall domain (epoch_anchor + ticks),
    # identical to the row timestamps written into the window cache.
    if noFault:
        # Pure nominal run: no fault onset/ESD semantics to record, and the
        # meta.json must not clobber a real scenario anchor in the same dir.
        print("[capture] --no-fault: pure nominal run; archive only "
              "(no meta.json written). Usable as held-out capture via "
              "dataset.heldout_source='capture'.")
        return final_archive, None

    fault_tick = int(round(scenario.get("fault_injection_tick", 723) * period_scale))
    fault_onset_ts = epoch_anchor + fault_tick * ms_per_tick

    esd_tick = parseEsdTick(sim_output)
    esd_ts = epoch_anchor + esd_tick * ms_per_tick if esd_tick is not None else None
    if esd_tick is not None:
        print(f"[capture] ESD detected at tick {esd_tick} (sim hour {esd_tick / period_scale:.2f})")
    else:
        print("[capture] Run completed without ESD trip.")
    meta = {
        "fault_code": int(scenario.get("fault_code", 9)),
        "fault_name": scenario.get("fault_name", "FLARE_BLOCKED"),
        "scenario": scenario,
        "ticks_per_day": ticks_per_day,
        "fault_onset_tick": fault_tick,
        "fault_onset_ts": fault_onset_ts,
        # ISO strings: naive render of the DTL wall domain, numerically equal
        # to fault_onset_ts (2000-01-01 00:00 naive epoch = epoch_anchor).
        "fault_onset_iso": datetime.fromtimestamp(
            fault_onset_ts / 1000.0, tz=timezone.utc).isoformat(),
        "esd_tick": esd_tick,
        "esd_ts": esd_ts,
        "esd_iso": datetime.fromtimestamp(
            esd_ts / 1000.0, tz=timezone.utc).isoformat() if esd_ts else None,
        "wal_anchor_ms": wal_anchor_ms,
        "archive_file": final_archive.name,
        "archive_bytes": final_archive.stat().st_size,
        "generated_at": datetime.now(timezone.utc).isoformat(),
    }

    meta_path = out_dir / "meta.json"
    meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"[capture] Metadata written: {meta_path}")

    return final_archive, meta_path


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(
        description="FLARE_BLOCKED WAL capture — verbatim s7shell simulation scripted via env vars")
    parser.add_argument("--out", required=True, help="Output directory for archive and meta.json")
    parser.add_argument("--config", default=None,
                        help="Config file (YAML/JSON); default: experiment dir config.yaml")
    parser.add_argument("--port", type=int, default=8100 + (os.getpid() % 900),
                        help="Gateway HTTP/WS port (gateway path)")
    parser.add_argument("--local", action="store_true",
                        help="In-shell capture via the Persistence binding (binary WAL, no gateway)")
    parser.add_argument("--no-fault", action="store_true",
                        help="Pure nominal run (fault_code forced to 0) for held-out "
                             "evaluation sets; writes flare_history_normal.jsonl.zst "
                             "and no scenario meta.json")
    parser.add_argument("--verbose", action="store_true",
                        help="Persist subprocess logs (gateway.log / s7shell.log) into the capture dir")
    args = parser.parse_args()

    try:
        runCapture(args)
    except Exception as exc:
        print(f"[capture] FAILED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
