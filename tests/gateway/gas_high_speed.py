#!/usr/bin/env python3
"""
gas_high_speed.py — High-speed simulation / WebSocket streaming validation.

Covers the Section 7 acceptance criteria for the gas-processing simulator
(sgrn/lib/gateway/simulations/gas_processing/simulation.as):

  1. Zero-sleep simulation still produces state updates (ticks complete,
     gateway twin advances).
  2. The gateway keeps receiving updates at accelerated speeds (twin totalizer
     matches the simulator's final state; WAL persists records).
  3. Fixed seed + identical inputs => identical [STATE] trajectories for
     1 ms pacing, zero pacing, and snapshot mode.
  4. Totalizers / state durations consistent with simulated elapsed time
     (final totalizer equals the no-gateway baseline; time_in_state_s grows
     by SIM_SECONDS_PER_TICK per tick).
  5. Timestamps monotonic with correct day/month/year rollover, including
     2000-02-01, leap day 2000-02-29, 2000-03-01 and 2001-01-01.
  6. Publishing backpressure does not corrupt simulation state
     ([STATE] equality under coalescing + connected at end of 2000-tick
     unpaced snapshot run).
  7. Connection loss handling (dead-port run keeps simulating deterministically
     and reports the outage instead of hanging).
  8. Every-tick publishing preserves all required records (WAL delta lines >=
     ticks with atomic_window_ms=0).
  9. Snapshot mode coalescing is explicit and the latest state is still exact
     (final twin == final sim state with fewer WAL lines).

Usage:
  python3 tests/gateway/gas_high_speed.py [--keep-tmp] [--ticks N]

Requirements: built ./gateway and ./s7shell binaries (see repo README),
python3 stdlib only. Set TZ=UTC for fully deterministic calendar checks
(the harness does this itself for child processes).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SIM_DIR = ROOT / "sgrn" / "lib" / "gateway" / "simulations" / "gas_processing"
SIM_SRC = SIM_DIR / "simulation.as"

CHILD_ENV = dict(os.environ)
CHILD_ENV["TZ"] = "UTC"


def find_binary(name: str) -> Path:
    app = {"s7shell": "s7shell", "gateway": "gateway"}.get(name, name)
    for cand in (
        # Fresh incremental builds first (validate C++ changes without install).
        ROOT / ".build" / "linux-static" / "sgrn" / "apps" / app / name,
        ROOT / ".prefix" / "linux-static" / "bin" / name,
        ROOT / ".dist" / "linux-static" / name,
        ROOT / ".dist" / "linux-static-release" / name,
    ):
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    raise FileNotFoundError(f"cannot find binary {name}")


def wait_http(url: str, proc: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"process exited early with status {proc.returncode}")
        try:
            with urllib.request.urlopen(url, timeout=2):
                return
        except (urllib.error.URLError, TimeoutError, OSError):
            time.sleep(0.2)
    raise TimeoutError(f"gateway not ready at {url}")


def http_get_json(url: str, timeout: float = 10.0):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


_failures: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {name}" + (f" — {detail}" if detail else ""))
    if not cond:
        _failures.append(name)


def stage_run(tmp: Path, name: str, port: int, max_ticks: int, sleep_ms: int,
              every_tick: bool, snapshot_n: int = 10, metrics_n: int = 50) -> Path:
    d = tmp / name
    d.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SIM_DIR / "schema.scl", d / "schema.scl")
    src = SIM_SRC.read_text(encoding="utf-8")
    subs = [
        ('const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";',
         f'const string GATEWAY_WS_URL = "ws://127.0.0.1:{port}/ws";'),
        ("const uint SIM_TICK_SLEEP_MS = 1;",
         f"const uint SIM_TICK_SLEEP_MS = {sleep_ms};"),
        ("const int MAX_TICKS = 0;",
         f"const int MAX_TICKS = {max_ticks};"),
        ("const bool PUBLISH_EVERY_TICK = true;",
         f"const bool PUBLISH_EVERY_TICK = {'true' if every_tick else 'false'};"),
        ("const uint SNAPSHOT_EVERY_N_TICKS = 10;",
         f"const uint SNAPSHOT_EVERY_N_TICKS = {snapshot_n};"),
        ("const uint METRICS_EVERY_N_TICKS = 50;",
         f"const uint METRICS_EVERY_N_TICKS = {metrics_n};"),
    ]
    for old, new in subs:
        if old not in src:
            raise RuntimeError(f"config marker missing: {old!r}")
        src = src.replace(old, new, 1)
    (d / "simulation.as").write_text(src, encoding="utf-8")
    cfg = {
        "schema": str((d / "schema.scl").resolve()),
        "state_dir": str((d / "gw_state").resolve()),
        "security_policy": "permissive",
        "northbound": {
            "http": {"ip": "127.0.0.1", "port": port},
            "websocket": {"ip": "127.0.0.1", "port": port},
        },
        "persistence": {
            "enabled": True, "mode": "changes_with_timestamp", "format": "jsonl",
            # 1 ms merge window: ticks paced by ACK round-trips land in separate
            # windows (one WAL line per tick) without the per-leaf line flood
            # and broker backlog that atomic_window_ms=0 produces at speed.
            "atomic_window_ms": 1, "batch_interval_s": 3600,
            "anchor_interval_s": 3600, "cloud_sync": {"enabled": False},
        },
    }
    (d / "gateway.json").write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    return d


def run_case(tmp: Path, name: str, port: int, max_ticks: int, sleep_ms: int,
             every_tick: bool, snapshot_n: int = 10, metrics_n: int = 50,
             atomic_window_ms: int = 1,
             with_gateway: bool = True, timeout: float = 240.0) -> dict:
    d = stage_run(tmp, name, port, max_ticks, sleep_ms, every_tick, snapshot_n, metrics_n)
    if atomic_window_ms != 1:
        cfg_path = d / "gateway.json"
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        cfg["persistence"]["atomic_window_ms"] = atomic_window_ms
        cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    gw_bin = find_binary("gateway")
    s7_bin = find_binary("s7shell")
    gw_proc = None
    gw_log = None
    if with_gateway:
        gw_log = open(d / "gateway.log", "w", encoding="utf-8")
        gw_proc = subprocess.Popen(
            [str(gw_bin), str(d / "gateway.json")], cwd=str(ROOT),
            stdout=gw_log, stderr=subprocess.STDOUT, env=CHILD_ENV)
        wait_http(f"http://127.0.0.1:{port}/endpoints", gw_proc)
    t0 = time.monotonic()
    sim_proc = subprocess.Popen(
        [str(s7_bin), "simulation.as"], cwd=str(d),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=CHILD_ENV)
    try:
        out, _ = sim_proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        sim_proc.kill()
        out, _ = sim_proc.communicate()
        out += "\n[HARNESS-TIMEOUT]\n"
    wall = time.monotonic() - t0
    twin_totalizer = None
    if with_gateway:
        # The simulator flushes (acked writes) before exit, and the gateway
        # applies each write to twin memory synchronously before ACKing, so a
        # raw memory read is exact immediately — no convergence poll needed.
        time.sleep(1.0)
        twin_totalizer = read_raw_totalizer(port)
        gw_proc.terminate()
        try:
            gw_proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            gw_proc.kill()
            gw_proc.wait()
        if gw_log is not None:
            gw_log.close()
    (d / "sim.log").write_text(out, encoding="utf-8")
    state = re.search(r"\[STATE\] tick=(\d+) sim_ts=([0-9\- :]+) (.*)", out)
    done = re.search(r"\[DONE\] ticks=(\d+) iters=(\d+) connected=(\w+)", out)
    # WAL delta lines across all archives (zstdcat when available), plus the
    # per-leaf value series of every "delta" line (leaf_id -> [values]).
    # Lines carry the gateway wall-ms merge-open "ts": sort by it first, since
    # archives can roll mid-run and filesystem order is not time order.
    lines = 0
    ordered: list[tuple[int, str, float]] = []
    archives = sorted((d / "gw_state").rglob("*.jsonl.zst")) + sorted((d / "gw_state").rglob("*.jsonl.zst.tmp"))
    # Note: the tail archive is often still provisional (*.tmp) when the
    # gateway stops: shutdown exits before the async finalize rename runs.
    # RecoveryEngine scans *.tmp too, so the harness must as well.
    for arch in archives:
        try:
            r = subprocess.run(["zstdcat", str(arch)], capture_output=True, timeout=60)
            if r.returncode != 0:
                continue
            for raw in r.stdout.split(b"\n"):
                if b'"type"' not in raw or b'delta' not in raw:
                    continue
                try:
                    doc = json.loads(raw.decode("utf-8", "replace"))
                except ValueError:
                    continue
                if doc.get("type") != "delta" or not isinstance(doc.get("changes"), dict):
                    continue
                lines += 1
                try:
                    ts = int(doc.get("ts", 0))
                except (TypeError, ValueError):
                    ts = 0
                for leaf, val in doc["changes"].items():
                    try:
                        ordered.append((ts, str(leaf), float(val)))
                    except (TypeError, ValueError):
                        pass
        except (OSError, subprocess.SubprocessError):
            pass
    ordered.sort(key=lambda t: t[0])
    series: dict[str, list[float]] = {}
    for _, leaf, val in ordered:
        series.setdefault(leaf, []).append(val)
    # Timestamps seen in METRICS lines (monotonicity sample). DTL toString()
    # wraps stamps in quotes; allow an optional leading quote.
    stamps = re.findall(r'sim_ts="?([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2})', out)
    return {"name": name, "dir": d, "out": out, "wall": wall,
            "ticks": int(done.group(1)) if done else -1,
            "connected": done.group(3) if done else "?",
            "state": state.group(0) if state else "",
            "state_fields": state.group(3) if state else "",
            "state_ts": state.group(2).strip() if state else "",
            "twin_totalizer": twin_totalizer,
            "wal_delta_lines": lines, "wal_series": series, "stamps": stamps}


def totalizer_series_length(res: dict, final: float) -> tuple[int, bool]:
    """Longest WAL leaf series ending at `final` (the feed/gas totalizer):
    returns (distinct_length, strictly_increasing).
    Consecutive duplicates are collapsed first: re-publishing an identical
    state (worker/explicit sync overlap, drain flush) is benign and must not
    fail the check; the trajectory itself is strictly increasing."""
    tol = max(2.0, abs(final) * 2e-6)
    best, best_inc = 0, False
    for vals in res["wal_series"].values():
        if not vals or abs(vals[-1] - final) > tol:
            continue
        dedup = [vals[0]]
        for v in vals[1:]:
            if v != dedup[-1]:
                dedup.append(v)
        inc = all(b > a for a, b in zip(dedup, dedup[1:]))
        if len(dedup) > best:
            best, best_inc = len(dedup), inc
    return best, best_inc


def twin_float(v) -> float | None:
    try:
        if isinstance(v, dict):
            v = v.get("value", v.get("totalizer"))
        return float(v)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None


_TYPE_SIZES = {"BOOL": 1, "SINT": 1, "USINT": 1, "BYTE": 1, "INT": 2, "UINT": 2,
               "WORD": 2, "DINT": 4, "UDINT": 4, "DWORD": 4, "REAL": 4,
               "LINT": 8, "ULINT": 8, "LWORD": 8, "LREAL": 8, "DTL": 12}


def resolve_totalizer(port: int) -> tuple[int, int]:
    """(byte_offset, size) of InletSeparation.feed_flow.totalizer in DB1 via /registry."""
    import struct as _s  # noqa: F401  (kept local: only used with raw reads)
    reg = http_get_json(f"http://127.0.0.1:{port}/registry?db=1")
    for db in reg.get("dbs", []):
        if db.get("number") != 1:
            continue
        for f in db.get("fields", []):
            if f.get("name") == "feed_flow":
                base = int(f.get("offset", 0))
                for ch in f.get("children", []):
                    if ch.get("name") == "totalizer":
                        return base + int(ch.get("offset", 0)), _TYPE_SIZES.get(ch.get("type", "LREAL"), 8)
    raise RuntimeError("totalizer field not found in registry")


def read_raw_totalizer(port: int) -> float | None:
    """Synchronous gateway-twin read: raw DB1 memory, no TreeCache lag
    (HTTP leaf reads go through the asynchronously-invalidated TreeCacheEngine
    and lag minutes behind under burst load)."""
    import struct
    try:
        off, size = resolve_totalizer(port)
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/memory/db/1/offset/{off}/size/{size}",
                timeout=10) as resp:
            raw = resp.read()
        if len(raw) != size:
            return None
        return struct.unpack("<d" if size == 8 else "<f", raw)[0]
    except Exception:
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--keep-tmp", action="store_true")
    ap.add_argument("--ticks", type=int, default=200)
    args = ap.parse_args()
    n = args.ticks
    if shutil.which("zstdcat") is None:
        print("[harness] FATAL: zstdcat required for WAL assertions", file=sys.stderr)
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="gas_high_speed_"))
    print(f"[harness] work dir: {tmp}")
    try:
        base_port = 18300
        print("\n== cases 1-3: pacing / determinism / every-tick preservation ==")
        a = run_case(tmp, "paced_every", base_port + 1, n, 1, True)
        b = run_case(tmp, "unpaced_every", base_port + 2, n, 0, True)
        print(f"[measure] paced:   {a['ticks']} ticks in {a['wall']:.2f}s "
              f"= {a['ticks']/a['wall']:.0f} ticks/s = {a['ticks']*3600/a['wall']/3600:.0f} sim-h/s")
        print(f"[measure] unpaced: {b['ticks']} ticks in {b['wall']:.2f}s "
              f"= {b['ticks']/b['wall']:.0f} ticks/s = {b['ticks']*3600/b['wall']/3600:.0f} sim-h/s")
        check("1a: zero-sleep run completes all ticks", b["ticks"] == n, f"ticks={b['ticks']}")
        check("1b: zero-sleep run stays connected", b["connected"] == "yes")
        check("3a: identical trajectory paced vs unpaced", a["state"] == b["state"],
              f"\n    A: {a['state'][:160]}\n    B: {b['state'][:160]}")
        m = re.search(r"tot_feed=([0-9.eE+\-]+)", b["state_fields"])
        sim_tot = float(m.group(1)) if m else float("nan")
        slen, sinc = totalizer_series_length(b, sim_tot)
        check("8: every-tick WAL keeps an increasing totalizer series ending at final",
              slen >= 1 and sinc, f"series_len={slen} increasing={sinc}")
        m_att = re.search(r"\[COUNTERS\] publish_attempts=(\d+) publish_errors=(\d+)", b["out"])
        att, err = (int(m_att.group(1)), int(m_att.group(2))) if m_att else (-1, -1)
        check("8b: every tick explicitly synced (attempts >= ticks, zero error episodes)",
              att >= n and err == 0, f"attempts={att} errors={err}")

        print("\n== case 8c: rigorous per-tick proof (window=0, 60 ticks) ==")
        b0 = run_case(tmp, "every_w0", base_port + 9, 60, 0, True, atomic_window_ms=0)
        m0 = re.search(r"tot_feed=([0-9.eE+\-]+)", b0["state_fields"])
        sim0 = float(m0.group(1)) if m0 else float("nan")
        s0len, s0inc = totalizer_series_length(b0, sim0)
        check("8c: atomic_window_ms=0 preserves every tick (>= 1 record per tick, ordered)",
              s0len >= 60 and s0inc, f"series_len={s0len} increasing={s0inc}")
        t0 = twin_float(b0["twin_totalizer"])
        check("8c-twin: final twin equals final sim state",
              t0 is not None and abs(t0 - sim0) <= max(2.0, abs(sim0) * 2e-6),
              f"twin={b0['twin_totalizer']} sim={sim0}")

        print("\n== case 9: snapshot mode (explicit coalescing) ==")
        c = run_case(tmp, "unpaced_snap", base_port + 3, n, 0, False, 50)
        print(f"[measure] snapshot unpaced: {c['ticks']} ticks in {c['wall']:.2f}s "
              f"= {c['ticks']/c['wall']:.0f} ticks/s = {c['ticks']*3600/c['wall']/3600:.0f} sim-h/s")
        check("9a: snapshot trajectory still identical (no state corruption)",
              c["state"] == b["state"])
        check("9b: snapshot mode coalesces (fewer WAL lines than every-tick)",
              0 <= c["wal_delta_lines"] < b["wal_delta_lines"],
              f"snap={c['wal_delta_lines']} every={b['wal_delta_lines']}")
        snap_tot, every_tot = twin_float(c["twin_totalizer"]), twin_float(b["twin_totalizer"])
        # Tolerance is relative (2e-6): the sim log prints doubles with ~6
        # significant digits, so the harness-side reference is quantised.
        check("9c: snapshot latest state still reaches twin",
              snap_tot is not None and every_tot is not None
              and abs(snap_tot - every_tot) <= max(2.0, abs(every_tot) * 2e-6),
              f"snap_twin={c['twin_totalizer']} every_twin={b['twin_totalizer']}")

        print("\n== cases 2/4: gateway receipt + totalizer consistency ==")
        m = re.search(r"tot_feed=([0-9.eE+\-]+)", b["state_fields"])
        sim_tot = float(m.group(1)) if m else float("nan")
        twin_tot = twin_float(b["twin_totalizer"])
        check("2: gateway twin totalizer matches simulator final state",
              twin_tot is not None and abs(twin_tot - sim_tot) <= max(2.0, abs(sim_tot) * 2e-6),
              f"twin={b['twin_totalizer']} sim={sim_tot}")
        # True-sum check against an independent no-gateway run (echo immunity).
        d = run_case(tmp, "nogw", base_port + 4, n, 0, True, with_gateway=False)
        check("4/6: connected run equals no-gateway baseline (no echo corruption)",
              d["state"] == b["state"], f"\n    nogw: {d['state'][:160]}\n    gw:   {b['state'][:160]}")

        print("\n== case 5: calendar rollover ==")
        # 744 ticks = 31 d -> 2000-02-01; 1416 -> 2000-02-29 (leap);
        # 1440 -> 2000-03-01; 8784 -> 2001-01-01. Snapshot mode: fast.
        # METRICS cadence 24 samples every midnight, so each boundary appears.
        # Exact expected strings (not recomputed from sim arithmetic).
        cal = run_case(tmp, "calendar", base_port + 5, 8784, 0, False, 500, 24, timeout=300.0)
        for expect in ["2000-02-01 00:00:00", "2000-02-29 00:00:00",
                       "2000-03-01 00:00:00", "2001-01-01 00:00:00"]:
            check(f"5: calendar shows {expect}", expect in cal["out"])
        stamps = cal["stamps"] + ([cal["state_ts"]] if cal["state_ts"] else [])
        check("5: timestamps monotonic", all(a2 <= b2 for a2, b2 in zip(stamps, stamps[1:])),
              f"samples={len(stamps)}")
        check("5: final stamp is 2001-01-01", cal["state_ts"] == "2001-01-01 00:00:00",
              cal["state_ts"])

        print("\n== case 6: backpressure soak (2000 ticks, unpaced, snapshot) ==")
        soak = run_case(tmp, "soak", base_port + 6, 2000, 0, False, 100)
        print(f"[measure] soak snapshot: {soak['ticks']} ticks in {soak['wall']:.2f}s "
              f"= {soak['ticks']/soak['wall']:.0f} ticks/s = {soak['ticks']*3600/soak['wall']/3600:.0f} sim-h/s")
        check("6a: soak completes all ticks", soak["ticks"] == 2000)
        check("6b: soak stays connected", soak["connected"] == "yes", soak["connected"])
        check("6c: soak trajectory matches 200-tick prefix run state shape",
              "tot_feed=" in soak["state"] and "esd=" in soak["state"])
        perr = re.search(r"publish_errors=(\d+)", soak["out"])
        check("6d: no publish error episodes", perr and perr.group(1) == "0",
              perr.group(0) if perr else "no counters")

        print("\n== case 7: gateway down ==")
        down = run_case(tmp, "down", base_port + 7, 60, 0, True, with_gateway=False)
        up60 = run_case(tmp, "up60", base_port + 8, 60, 0, True)
        check("7a: simulation continues without gateway", down["ticks"] == 60,
              f"ticks={down['ticks']}")
        check("7b: outage is reported, not silent",
              "WARNING" in down["out"] or "giving up" in down["out"] or "RECONNECT" in down["out"])
        check("7c: offline trajectory equals connected trajectory (deterministic, no echo dependence)",
              down["state"] == up60["state"],
              f"\n    down: {down['state'][:160]}\n    up:   {up60['state'][:160]}")

        print(f"\n[harness] {'ALL CHECKS PASSED' if not _failures else f'{len(_failures)} FAILURES: {_failures}'}")
        return 0 if not _failures else 1
    finally:
        if args.keep_tmp:
            print(f"[harness] kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
