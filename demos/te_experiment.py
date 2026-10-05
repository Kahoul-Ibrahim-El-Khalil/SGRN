#!/usr/bin/env python3
"""TE evaluation through the gateway (orchestrator + metrics + baseline).

SIMULATED DATA ONLY. Not a live plant, not real-plant validation.

For each archive: launches its own sgrn_replay subprocess in headless
replay mode (sgrn_replay -a ARCHIVE -s SCHEMA --http-port PORT), waits for
GET /endpoints, sets speed via POST /replay/speed (same mechanism as the
dashboard), collects samples via GET /data/TE in replay order, then shuts
down cleanly (including on exceptions / Ctrl+C).

Evaluation data goes through the gateway; this script never reads RData.
Training reads converted data offline (see te_train.py).

Metrics: false-alarm rate on held-out fault-free runs, detection delay
(samples after true onset), detection rate per fault 1-20. Onsets
(1-indexed sample): train 21, test 161. Faults 3/9/15 are hard; reported
as-is, no tuning around them.

Baseline: sklearn IsolationForest on the same splits with the same
split-conformal rule. NOTE: sgrn.python.sgrn.ml.trainer.AutoMLTrainer
task="anomaly" hardcodes contamination=0.05 (trainer.py) and has no
threshold calibration, so it cannot be used here.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

FEATURES = [
    "a_feed_flow", "d_feed_flow", "e_feed_flow", "ac_feed_flow", "recycle_flow",
    "reactor_feed_rate", "purge_rate", "sep_underflow", "stripper_underflow",
    "stripper_steam_flow", "reactor_pressure", "sep_pressure", "stripper_pressure",
    "reactor_level", "sep_level", "stripper_level", "reactor_temp", "sep_temp",
    "stripper_temp", "reactor_cw_temp", "sep_cw_temp", "compressor_work",
    "feed_comp_a", "feed_comp_b", "feed_comp_c", "feed_comp_d", "feed_comp_e",
    "feed_comp_f", "purge_comp_a", "purge_comp_b", "purge_comp_c", "purge_comp_d",
    "purge_comp_e", "purge_comp_f", "purge_comp_g", "purge_comp_h",
    "product_comp_d", "product_comp_e", "product_comp_f", "product_comp_g",
    "product_comp_h", "d_feed_valve", "e_feed_valve", "a_feed_valve",
    "ac_feed_valve", "recycle_valve", "purge_valve", "sep_pot_valve",
    "product_valve", "steam_valve", "reactor_cw_valve", "condenser_valve",
]  # twin leaf names under DB "TE"; order matches te_rieth/schema.scl
ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "sgrn/lib/gateway/simulations/te_rieth/schema.scl"
TEST_ONSET = 161
TRAIN_ONSET = 21


def http_get(url: str, timeout: float = 5.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def http_post_json(url: str, payload: dict, timeout: float = 5.0):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "{}")


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = int(s.getsockname()[1])
    s.close()
    return port


def find_replay() -> Path:
    candidates = (
        ROOT / ".dist/linux-static-release/sgrn_replay",
        ROOT / ".dist/linux-static/sgrn_replay",
        ROOT / ".prefix/bin/sgrn_replay",
        ROOT / ".build/linux-static-release/sgrn/apps/replay/sgrn_replay",
        ROOT / ".build/linux-static-release/sgrn_replay",
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which("sgrn_replay")
    if found:
        return Path(found)
    raise FileNotFoundError("Cannot find sgrn_replay; build/install the Linux binaries first.")


def wait_ready(base: str, proc: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"replay exited early with status {proc.returncode}")
        try:
            http_get(base + "/endpoints", timeout=1)
            return
        except (urllib.error.URLError, TimeoutError, OSError):
            time.sleep(0.2)
    raise TimeoutError(f"gateway did not become ready at {base}/endpoints")


def stop_proc(proc: subprocess.Popen | None, timeout: float = 10.0) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def collect_run(archive: Path, speed: float | str, n_samples: int, log_path: Path) -> list[list[float]]:
    """Replay one archive via a private gateway; return rows in replay order."""
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    replay = find_replay()
    args = [str(replay), "-a", str(archive), "-s", str(SCHEMA), "--http-port", str(port)]
    if isinstance(speed, str) and speed == "unpaced":
        args += ["-n"]
    else:
        args += ["-r", str(float(speed))]
    log_path.parent.mkdir(parents=True, exist_ok=True)
    rows: list[list[float]] = []
    with open(log_path, "w", encoding="utf-8") as log:
        proc = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_ready(base, proc)
            # Same mechanism as the dashboard control.
            try:
                if isinstance(speed, str):
                    http_post_json(base + "/replay/speed", {"speed": "unpaced"})
                else:
                    http_post_json(base + "/replay/speed", {"speed": float(speed)})
            except Exception as exc:
                print(f"[warn] /replay/speed unavailable, continuing at launch speed: {exc}")
            last_frames = -1
            deadline = time.monotonic() + max(60.0, n_samples * 0.5)
            while len(rows) < n_samples and time.monotonic() < deadline:
                if proc.poll() is not None and len(rows) >= n_samples:
                    break
                try:
                    status = json.loads(http_get(base + "/replay/status", timeout=2).decode() or "{}")
                    frames = int(status.get("frames", -1))
                except Exception:
                    frames = -1
                try:
                    snap = json.loads(http_get(base + "/data/TE", timeout=5).decode() or "{}")
                except Exception:
                    time.sleep(0.05)
                    continue
                try:
                    row = [float(snap[c]) for c in FEATURES]
                except (KeyError, TypeError, ValueError):
                    time.sleep(0.05)
                    continue
                if frames != last_frames or not rows or rows[-1] != row:
                    last_frames = frames
                    rows.append(row)
                time.sleep(0.01)
            if len(rows) < n_samples:
                print(f"[warn] {archive.name}: collected {len(rows)}/{n_samples} rows via {base}")
            return rows
        finally:
            stop_proc(proc)


def load_model(model_dir: Path):
    import numpy as np
    import torch

    cfg = json.loads((model_dir / "config.json").read_text())
    scaler = np.load(model_dir / "scaler.npz")
    mu = scaler["mean"].astype(np.float64)
    sd = scaler["std"].astype(np.float64)

    import torch.nn as nn

    model = nn.Sequential(
        nn.Linear(52, 32), nn.ReLU(), nn.Linear(32, 16), nn.ReLU(), nn.Linear(16, 8), nn.ReLU(),
        nn.Linear(8, 16), nn.ReLU(), nn.Linear(16, 32), nn.ReLU(), nn.Linear(32, 52),
    )
    model.load_state_dict(torch.load(model_dir / "ae.pt", map_location="cpu"))
    model.eval()
    return model, mu, sd, float(cfg["threshold"]), cfg


def recon_errors(model, mu, sd, rows) -> "np.ndarray":
    import numpy as np
    import torch

    x = np.asarray(rows, dtype=np.float64)
    xn = ((x - mu) / sd).astype(np.float32)
    with torch.no_grad():
        err = ((model(torch.from_numpy(xn)) - torch.from_numpy(xn)) ** 2).mean(dim=1).numpy()
    return err


def parse_archive_name(path: Path) -> tuple[int, int]:
    stem = path.stem.split(".")[0]
    return int(stem.split("_")[2][1:]), int(stem.split("_")[3][3:])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--indir", required=True, help="converted test archives dir")
    ap.add_argument("--model", required=True, help="te_train.py output dir")
    ap.add_argument("--out", required=True, help="results CSV path")
    ap.add_argument("--plot", default=None, help="example error-vs-time PNG")
    ap.add_argument("--speed", default="1800", help="launch speed or 'unpaced'")
    ap.add_argument("--faults", type=int, nargs="*", default=None)
    ap.add_argument("--max-runs", type=int, default=5, help="runs per fault to stream")
    ap.add_argument("--alpha", type=float, default=0.01)
    args = ap.parse_args()

    try:
        speed: float | str = "unpaced" if args.speed == "unpaced" else float(args.speed)
    except ValueError:
        print("invalid --speed", file=sys.stderr)
        return 2

    indir = Path(args.indir)
    model_dir = Path(args.model)
    archives = sorted(indir.glob("*.jsonl.zst"))
    if args.faults is not None:
        archives = [p for p in archives if parse_archive_name(p)[0] in set(args.faults)]
    by_fault: dict[int, list[Path]] = {}
    for p in archives:
        f, _ = parse_archive_name(p)
        by_fault.setdefault(f, []).append(p)
    for f in by_fault:
        by_fault[f] = sorted(by_fault[f])[: max(1, args.max_runs)]
    if not by_fault:
        print(f"no archives in {indir}", file=sys.stderr)
        return 1

    model, mu, sd, threshold, cfg = load_model(model_dir)
    import numpy as np
    from sklearn.ensemble import IsolationForest

    # Baseline IF on same train features is fitted offline, but its threshold
    # uses the same split-conformal rule on calibration scores (AutoMLTrainer
    # cannot do this; contamination is hardcoded there).
    train_dir = indir  # overwritten below if --train-indir style layout differs
    print(f"model threshold={threshold:.6f} (alpha={cfg.get('alpha')})")

    run_dir = Path(args.out).parent / "replay_logs"
    per_fault: dict[int, dict] = {}
    far_rows = 0
    far_flags = 0
    example: tuple | None = None

    # Fit baseline once from fault-free archives in the same indir (f00).
    ff = sorted([p for p in indir.glob("*_f00_*.jsonl.zst")])[:4]
    if not ff:
        print("baseline needs fault-free archives (*_f00_*) in --indir", file=sys.stderr)
        return 1
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from te_train import decode_archive as decode_offline

    x_base = np.concatenate([decode_offline(p) for p in ff], axis=0)
    xb = (x_base - mu) / sd
    iso = IsolationForest(n_estimators=200, random_state=0)
    iso.fit(xb.astype(np.float64))
    cal = sorted([p for p in indir.glob("*_f00_*.jsonl.zst")])[4:6]
    if not cal:
        print("baseline needs 2 held-out fault-free archives for calibration", file=sys.stderr)
        return 1
    xc = np.concatenate([decode_offline(p) for p in cal], axis=0)
    xc = ((xc - mu) / sd).astype(np.float64)
    s_cal = -iso.score_samples(xc)
    n = len(s_cal)
    k = min(max(int(np.ceil((n + 1) * (1 - args.alpha))), 1), n)
    iso_thr = float(np.sort(s_cal)[k - 1])
    print(f"isolationforest threshold={iso_thr:.6f} n_cal={n}")

    try:
        for fault in sorted(by_fault):
            det_rates = []
            delays = []
            iso_rates = []
            for archive in by_fault[fault]:
                _, run = parse_archive_name(archive)
                n_exp = 960 if "test" in archive.name else 500
                onset = TEST_ONSET if n_exp == 960 else TRAIN_ONSET
                rows = collect_run(archive, speed, n_exp, run_dir / f"{archive.stem}.log")
                err = recon_errors(model, mu, sd, rows)
                flags = err > threshold
                xb_run = ((np.asarray(rows) - mu) / sd).astype(np.float64)
                iso_flags = (-iso.score_samples(xb_run)) > iso_thr
                if fault == 0:
                    far_rows += len(flags)
                    far_flags += int(flags.sum())
                else:
                    post = flags[onset - 1:]
                    det_rates.append(float(post.mean()) if len(post) else 0.0)
                    hit = np.flatnonzero(post)
                    delays.append(int(hit[0]) if len(hit) else -1)
                    ipost = iso_flags[onset - 1:]
                    iso_rates.append(float(ipost.mean()) if len(ipost) else 0.0)
                if example is None and fault != 0:
                    example = (fault, run, err, onset)
                print(f"f={fault:02d} run={run:03d} n={len(rows)} "
                      f"det={float(flags[onset-1:].mean()) if fault else float(flags.mean()):.3f}")
            if fault == 0:
                per_fault[fault] = {"far": far_flags / max(1, far_rows), "n": far_rows}
            else:
                per_fault[fault] = {
                    "det_rate": float(np.mean(det_rates)) if det_rates else 0.0,
                    "delay": float(np.mean([d for d in delays if d >= 0])) if any(d >= 0 for d in delays) else -1.0,
                    "iso_det_rate": float(np.mean(iso_rates)) if iso_rates else 0.0,
                    "n_runs": len(det_rates),
                }
    except KeyboardInterrupt:
        print("interrupted; partial results below")

    out_csv = Path(args.out)
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["fault", "metric", "ae", "isolationforest", "n"])
        for fault in sorted(per_fault):
            d = per_fault[fault]
            if fault == 0:
                w.writerow([0, "false_alarm_rate", f"{d['far']:.4f}", "", d["n"]])
            else:
                w.writerow([fault, "detection_rate", f"{d['det_rate']:.4f}", f"{d['iso_det_rate']:.4f}", d["n_runs"]])
                w.writerow([fault, "delay_samples", f"{d['delay']:.1f}", "", d["n_runs"]])
    print(f"wrote {out_csv}")
    print("fault | ae_det | iso_det | delay | n")
    for fault in sorted(per_fault):
        d = per_fault[fault]
        if fault == 0:
            print(f"  00  FAR={d['far']:.4f} (alpha={args.alpha}) n={d['n']}")
        else:
            print(f"  {fault:02d}  {d['det_rate']:.3f}  {d['iso_det_rate']:.3f}  {d['delay']:.1f}  {d['n_runs']}")

    if args.plot and example is not None:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fault, run, err, onset = example
        plt.figure()
        plt.plot(np.arange(1, len(err) + 1), err, label="recon error")
        plt.axhline(threshold, color="red", linestyle="--", label="conformal threshold")
        plt.axvline(onset, color="black", linestyle=":", label="true onset")
        plt.xlabel("sample")
        plt.ylabel("MSE")
        plt.title(f"Simulated TE f{fault:02d} run{run:03d} (simulated data only)")
        plt.legend()
        Path(args.plot).parent.mkdir(parents=True, exist_ok=True)
        plt.savefig(args.plot, dpi=120, bbox_inches="tight")
        print(f"wrote {args.plot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
