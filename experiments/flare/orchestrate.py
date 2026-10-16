#!/usr/bin/env python3
"""experiments/flare/orchestrate.py — End-to-end pipeline orchestrator for the Flare Blocked experiment.

Orchestrates the entire research lifecycle with rich tqdm progress visualization:
  [1/5] Process Telemetry & WAL Ingestion (flare_history.jsonl.zst)
  [2/5] SCL Schema Parsing & Sliding Window Extraction (dataset.csv)
  [3/5] Manifold Autoencoder Training & Split-Conformal Calibration (model.pt, model_meta.json)
  [4/5] 742-Hour Timeline Evaluation & Dimensional Attribution (evaluation_plot.png, evaluation_metrics.json)
  [5/5] Executive Diagnostic Report Generation (report.md)

Usage:
  python3 experiments/flare run
  python3 experiments/flare run --output-dir scratch/flare_run --epochs 50
  python3 experiments/flare run --capture

  # Direct (same as above):
  python3 experiments/flare/orchestrate.py --output-dir scratch/flare_run
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd
from tqdm import tqdm

# Ensure sgrn python modules are importable
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "sgrn/python") not in sys.path:
    sys.path.insert(0, str(ROOT / "sgrn/python"))

from config_loader import loadConfig, getConfigPath

from sgrn.ml import (
    Scaler,
    configureModelFromSchema,
    createSlidingWindows,
    getHardwareInfo,
    resolveDevice,
    trainManifoldDetector,
)
from sgrn.ml.manifold import ProcessManifoldDetector
from evaluate import computeEvaluationMetrics, epochMsUtc
from evaluate import computeEvaluationMetrics, epochMsUtc

import torch

FLARE_DIR = Path(__file__).resolve().parent
BASE_CONFIG_PATH = FLARE_DIR / "config.json"
BASE_META_PATH = FLARE_DIR / "meta.json"
BASE_ARCHIVE_PATH = FLARE_DIR / "flare_history.jsonl.zst"
SCL_SCHEMA_PATH = ROOT / "sgrn/lib/gateway/simulations/gas_processing/schema.scl"


def _getWindowSize(config: dict) -> int:
    """Extract window_size from config, supporting both legacy int and new dict format."""
    ws = config.get("dataset", {}).get("window_size", 12)
    if isinstance(ws, dict):
        return int(ws.get("size", 12))
    return int(ws)


def printBanner(output_dir: Path, device: torch.device, hw_info: dict) -> None:
    print("\n" + "=" * 80)
    print("      SGRN CYBER-PHYSICAL PLANT SUPERVISION — PIPELINE ORCHESTRATOR")
    print("=" * 80)
    print(f"  Target Output Dir:  {output_dir}")
    print(f"  Active Compute:     {device} ({hw_info['platform']} - {hw_info['backend']})")
    print(f"  SCL Schema:         {SCL_SCHEMA_PATH.relative_to(ROOT)}")
    print(f"  Time:               {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 80 + "\n")


def findSgrnDatasetBin() -> Path:
    paths = (
        ROOT / ".dist/linux-static-release/sgrn_dataset",
        ROOT / ".dist/linux-static/sgrn_dataset",
        ROOT / ".prefix/bin/sgrn_dataset",
    )
    for p in paths:
        if p.is_file() and os.access(p, os.X_OK):
            return p
    raise FileNotFoundError("sgrn_dataset binary not found; please build SGRN tools.")


def stage1Ingestion(output_dir: Path, run_capture: bool) -> Tuple[Path, Path]:
    """Ensure WAL archive and simulation metadata exist in output_dir."""
    print("▶ [1/5] Process Telemetry & WAL Ingestion")
    archive_out = output_dir / "flare_history.jsonl.zst"
    meta_out = output_dir / "meta.json"

    if run_capture:
        print("  Running physical process simulation via s7shell soft-PLC...")
        capture_script = FLARE_DIR / "run_capture.py"
        cmd = [sys.executable, str(capture_script)]
        pbar = tqdm(total=100, desc="  Capturing Soft-PLC Telemetry", bar_format="{l_bar}{bar:30}{r_bar}")
        res = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
        pbar.update(100)
        pbar.close()
        if res.returncode != 0:
            raise RuntimeError(f"run_capture.py failed:\n{res.stderr}")
        # Copy produced files to output_dir if needed
        if output_dir != FLARE_DIR:
            shutil.copy2(BASE_ARCHIVE_PATH, archive_out)
            shutil.copy2(BASE_META_PATH, meta_out)
    else:
        # Use existing archive
        if not archive_out.is_file():
            if not BASE_ARCHIVE_PATH.is_file():
                raise FileNotFoundError(f"Base WAL archive missing: {BASE_ARCHIVE_PATH}")
            print(f"  Copying base archive to {archive_out.name}...")
            shutil.copy2(BASE_ARCHIVE_PATH, archive_out)
        if not meta_out.is_file() and BASE_META_PATH.is_file():
            shutil.copy2(BASE_META_PATH, meta_out)

    size_mb = archive_out.stat().st_size / (1024 * 1024)
    print(f"  ✓ WAL Archive verified: {archive_out.name} ({size_mb:.2f} MB, compressed Zstd)")
    return archive_out, meta_out


from dataset import ensureDatasetCsv, getSplitsAndWindows, loadDatasetDf


def stage2Dataset(archive_path: Path, output_dir: Path, config: dict) -> Tuple[pd.DataFrame, pd.Series, dict]:
    """Convert WAL archive to CSV and extract chronological sliding windows."""
    print("\n▶ [2/5] SCL Schema Parsing & Sliding Window Extraction")
    csv_path = output_dir / "dataset" / "dataset.csv"

    meta_path = output_dir / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else json.loads(BASE_META_PATH.read_text(encoding="utf-8"))

    from dataset import ensureDatasetCsv

    ensureDatasetCsv(archive_path, csv_path)
    splits = getSplitsAndWindows(archive_path=archive_path, csv_path=csv_path, config=config, meta=meta)

    features = splits["features"]
    feature_df, timestamps = loadDatasetDf(csv_path, features)
    splits["feature_df"] = feature_df
    splits["timestamps"] = timestamps

    X_train = splits["X_train"]
    X_cal = splits["X_cal"]
    X_test = splits["X_test"]

    print(f"  ✓ Windows extracted: Train={X_train.shape[0]} | Cal={X_cal.shape[0]} | Fault Test={X_test.shape[0]}")
    print(f"  ✓ Features: {len(features)} process variables across 12-hour temporal windows")

    return feature_df, timestamps, splits


def stage3TrainAndCalibrate(
    splits: dict,
    output_dir: Path,
    config: dict,
    epochs: int,
    batch_size: int,
    lr: float,
    seed: int,
    device: torch.device,
) -> ProcessManifoldDetector:
    """Train WindowAutoencoder and compute split-conformal calibration thresholds."""
    print("\n▶ [3/5] Manifold Autoencoder Training & Split-Conformal Calibration")

    torch.manual_seed(seed)
    np.random.seed(seed)

    X_train = splits["X_train"]
    X_cal = splits["X_cal"]
    scaler = splits["scaler"]
    features = splits["features"]
    window_size = _getWindowSize(config)

    # Physical dimensions/subsystems from the authoritative schema parse.
    schema_cfg = configureModelFromSchema(SCL_SCHEMA_PATH, windowSize=window_size)
    schema_dims = schema_cfg.get("featureDimensions", {})
    schema_subs = schema_cfg.get("featureSubsystems", {})
    feature_dims = {f: schema_dims.get(f, "General") for f in features}
    feature_subs = {f: schema_subs.get(f, f.split(".", 1)[0] if "." in f else "General")
                    for f in features}

    model_cfg = dict(config.get("model", {}))
    model_cfg.update({"epochs": epochs, "batch_size": batch_size, "learning_rate": lr})
    detector = trainManifoldDetector(
        X_train=X_train,
        X_cal=X_cal,
        scaler=scaler,
        features=features,
        config={"model": model_cfg},
        seed=seed,
        device=device,
        featureDimensions=feature_dims,
        featureSubsystems=feature_subs,
    )

    detector.save(output_dir / "model")
    print(f"  ✓ Split-Conformal Calibration complete:")
    print(f"    - Bounded Threshold q_alpha (1 - alpha = {1 - detector.alpha:.2f}): {detector.q_alpha:.4f}")
    print(f"    - Upper Safety Bound q_upper (k={detector.k_multiplier:.1f}, heuristic): {detector.q_upper:.4f}")
    print(f"  ✓ Model weights & metadata saved to {output_dir / 'model'}")
    return detector



def stage4EvaluateAndPlot(
    detector: ProcessManifoldDetector,
    splits: dict,
    output_dir: Path,
    config: dict,
    make_plot: bool = True,
) -> dict:
    """Evaluate 742h timeline, attribution, and ESD lead time."""
    print("\n▶ [4/5] 742-Hour Timeline Evaluation & Dimensional Attribution")

    feature_df = splits["feature_df"]
    sim_timestamps = splits["timestamps"]
    meta = splits["meta"]
    window_size = detector.window_size

    try:
        fault_onset_dt = pd.to_datetime(meta["fault_onset_iso"])
        fault_onset_dt = (fault_onset_dt.tz_convert(None) if getattr(fault_onset_dt, "tzinfo", None)
                          else fault_onset_dt.tz_localize(None))
    except (KeyError, TypeError, ValueError) as e:
        raise ValueError(f"Invalid experiment metadata for evaluation: {e}") from e
    naive_ts = sim_timestamps.dt.tz_localize(None)
    pre_mask = naive_ts < fault_onset_dt
    pre_indices = np.where(pre_mask)[0]
    n_train = int(len(pre_indices) * config.get("dataset", {}).get("train_ratio", 0.70))
    cal_indices = pre_indices[n_train:]

    raw_values = feature_df.to_numpy(dtype=np.float32)

    # 1. False alarm rate on calibration windows
    cal_states = []
    for i in range(len(cal_indices) - window_size + 1):
        idx_slice = cal_indices[i : i + window_size]
        res = detector.detect(raw_values[idx_slice])
        cal_states.append(res["state"])

    cal_total = len(cal_states)
    cal_abnormal = sum(1 for s in cal_states if s == "abnormal")
    cal_uncertain = sum(1 for s in cal_states if s == "uncertain")
    far_total = (cal_abnormal + cal_uncertain) / max(1, cal_total)
    far_abnormal_only = cal_abnormal / max(1, cal_total)

    # 2. Sliding window scan across full 742-hour timeline
    total_eval_windows = len(raw_values) - window_size + 1
    all_scores = []
    all_states = []
    all_results = []
    all_times = []

    pbar = tqdm(total=total_eval_windows, desc="  Scanning 742h Timeline", bar_format="{l_bar}{bar:30}{r_bar}")
    for i in range(total_eval_windows):
        win = raw_values[i : i + window_size]
        res = detector.detect(win)
        ts = sim_timestamps.iloc[i + window_size - 1]
        all_scores.append(res["score"])
        all_states.append(res["state"])
        all_results.append(res)
        all_times.append(ts)
        pbar.update(1)
    pbar.close()

    # 3. Detection delay & ESD lead time (shared core: None-safe, empty-safe).
    # fault_onset_dt/ts and esd_ts were validated at the top of this stage.
    try:
        fault_onset_ts = int(meta["fault_onset_ts"])
    except (KeyError, TypeError, ValueError) as e:
        raise ValueError(f"Invalid experiment metadata for evaluation: {e}") from e
    esd_ts = meta.get("esd_ts")
    if esd_ts is not None:
        try:
            esd_ts = int(esd_ts)
        except (TypeError, ValueError) as e:
            raise ValueError(f"Invalid esd_ts {esd_ts!r}: {e}") from e
    metrics = computeEvaluationMetrics(
        all_scores, all_states, all_times, all_results,
        fault_onset_dt, fault_onset_ts, esd_ts,
        esd_iso=meta.get("esd_iso"), fault_onset_iso=meta.get("fault_onset_iso"),
        q_alpha=detector.q_alpha, q_upper=detector.q_upper,
    )
    metrics.update({
        "calibration_windows": cal_total,
        "false_alarm_rate_total": float(far_total),
        "false_alarm_rate_abnormal_only": float(far_abnormal_only),
        "primary_dimension": None,
        "dimension_breakdown": {},
    })
    first_abnormal_iso = metrics["first_abnormal_iso"]
    if first_abnormal_iso:
        for res, iso in zip(all_results, [t.isoformat() for t in all_times]):
            if iso == first_abnormal_iso:
                metrics["primary_dimension"] = res.get("primary_dimension")
                metrics["dimension_breakdown"] = res.get("dimension_breakdown", {})
                break

    metrics_path = output_dir / "evaluation_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(f"  ✓ Metrics saved: {metrics_path.name}")

    if make_plot:
        if not all_scores or len(sim_timestamps) == 0:
            print("  ✓ Diagnostic Plot skipped: no scored windows.")
            return metrics
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        first_iso = metrics["first_abnormal_iso"]
        first_abnormal_time = (pd.to_datetime(first_iso) if first_iso else None)
        first_abnormal_res = None
        first_score = None
        if first_iso:
            for res, t in zip(all_results, all_times):
                if t.isoformat() == first_iso:
                    first_abnormal_res = res
                    first_score = res.get("score")
                    break

        plot_path = output_dir / "evaluation_plot.png"
        fig, ax = plt.subplots(figsize=(14, 6), dpi=150)
        start_t = sim_timestamps.iloc[0]
        elapsed_hours = [(t - start_t).total_seconds() / 3600.0 for t in all_times]

        ax.plot(elapsed_hours, all_scores, label="Reconstruction MSE", color="#1f77b4", linewidth=1.5)
        ax.axhline(detector.q_alpha, color="#ff7f0e", linestyle="--", linewidth=1.2, label=f"q_alpha ({detector.q_alpha:.3f})")
        ax.axhline(detector.q_upper, color="#d62728", linestyle="--", linewidth=1.2, label=f"q_upper ({detector.q_upper:.3f})")

        max_y = min(150.0, max(all_scores) * 1.1) if all_scores else 1.0
        ax.set_ylim(-0.1, max_y)
        ax.axhspan(-0.1, detector.q_alpha, color="green", alpha=0.08, label="Normal Zone")
        ax.axhspan(detector.q_alpha, detector.q_upper, color="orange", alpha=0.10, label="Uncertain Zone")
        ax.axhspan(detector.q_upper, max_y, color="red", alpha=0.08, label="Abnormal Zone")

        fault_elapsed = (fault_onset_dt - start_t).total_seconds() / 3600.0
        ax.axvline(fault_elapsed, color="purple", linestyle="-.", linewidth=1.5, label=f"Fault Onset (t={fault_elapsed:.0f}h)")

        if esd_ts and first_abnormal_time:
            esd_dt = pd.to_datetime(meta.get("esd_iso"))
            esd_dt = (esd_dt.tz_convert(None) if getattr(esd_dt, "tzinfo", None)
                      else esd_dt.tz_localize(None))
            esd_elapsed = (esd_dt - start_t).total_seconds() / 3600.0
            ax.axvline(esd_elapsed, color="black", linestyle=":", linewidth=1.8, label=f"ESD Trip (t={esd_elapsed:.0f}h)")

            det_elapsed = (first_abnormal_time - start_t).total_seconds() / 3600.0
            ax.scatter([det_elapsed], [first_score], color="red", marker="*", s=220, zorder=5, label=f"Detection (t={det_elapsed:.0f}h)")

        ax.set_title("Flare Blocked Anomaly Detection — Reconstruction Score vs Time", fontsize=14, fontweight="bold", pad=12)
        ax.set_xlabel("Elapsed Simulation Time (Hours)", fontsize=11)
        ax.set_ylabel("Reconstruction Score", fontsize=11)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="upper left", framealpha=0.9)
        plt.tight_layout()
        plt.savefig(plot_path)
        plt.close(fig)
        print(f"  ✓ Diagnostic Plot saved: {plot_path.name}")

    return metrics


def stage5Report(metrics: dict, output_dir: Path) -> Path:
    """Generate Markdown executive report."""
    print("\n▶ [5/5] Executive Diagnostic Report Generation")
    report_path = output_dir / "report.md"

    dim_breakdown_rows = ""
    for feat, info in list(metrics.get("dimension_breakdown", {}).items())[:5]:
        err_val = info.get("error", info.get("error_sum", 0.0))
        sub_name = info.get("subsystem", "General")
        dim_breakdown_rows += f"| `{feat}` | {err_val:.4f} | {info['percentage']:.2f}% | {sub_name} |\n"

    def _status(ok: bool) -> str:
        return "**PASS**" if ok else "**FAIL**"

    def _hours(value) -> str:
        return f"{value:.1f} hours" if value is not None else "n/a (no detection)"

    far = metrics["false_alarm_rate_abnormal_only"]
    delay = metrics["detection_delay_hours"]
    lead = metrics["lead_time_before_esd_hours"]
    primary_feat = metrics.get("primary_dimension", "N/A")
    primary_sub = metrics.get("primary_subsystem", "SafetySystems")
    flare_top3 = bool(metrics["flare_header_press_in_top3"])
    feed_top3 = bool(metrics["feed_pressure_in_top3"])
    detected = metrics["first_abnormal_iso"] is not None

    report_md = f"""# SGRN Autonomous Supervision — Flare Blocked Anomaly Detection Report

**Generated:** {datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")}  
**Target Output Directory:** `{output_dir}`

---

## Executive Summary

The physics-informed split-conformal manifold detector was evaluated across the full 31-day (742-hour) natural gas molecular sieve plant simulation.

| Metric | Result | Target Benchmark | Status |
|:---|:---:|:---:|:---:|
| **False Alarm Rate (Abnormal Only)** | **{far * 100:.2f}%** | < 0.01% | {_status(far < 0.0001)} |
| **Detection Delay** | **{_hours(delay)}** | <= 2.0 hours | {_status(detected and delay is not None and delay <= 2.0)} |
| **Lead Time Before Emergency Shutdown (ESD)** | **{_hours(lead)}** | >= 12.0 hours | {_status(lead is not None and lead >= 12.0)} |
| **Primary Drifting Feature Dimension** | **`{primary_feat}`** | `SafetySystems.flare_header_press` | {_status(primary_feat == "SafetySystems.flare_header_press")} |
| **Primary Subsystem / Device** | **`{primary_sub}`** | `SafetySystems` | {_status(primary_sub == "SafetySystems")} |
| **Flare Header in Top-3 Deviations** | **{flare_top3}** | True | {_status(flare_top3)} |
| **Feed Pressure in Top-3 Deviations** | **{feed_top3}** | True | {_status(feed_top3)} |

---

## Split-Conformal Calibration Thresholds

- **Calibration Sample Size (n_cal)**: {metrics['calibration_windows']} 12-hour temporal windows
- **Conformal Threshold (q_alpha)**: `{metrics['q_alpha']:.4f}`
- **Upper Safety Bound (q_upper, heuristic multiplier, not a calibrated level)**: `{metrics['q_upper']:.4f}`

---

## Top Feature Dimension Deviations at Detection (t = {metrics['first_abnormal_iso']})

| Feature Dimension | Residual MSE | Contribution Percentage | Subsystem / Device |
|:---|:---:|:---:|:---:|
{dim_breakdown_rows}

Top-3 deviating process signals:
1. `{metrics['top_signals_at_detection'][0] if len(metrics['top_signals_at_detection']) > 0 else 'N/A'}`
2. `{metrics['top_signals_at_detection'][1] if len(metrics['top_signals_at_detection']) > 1 else 'N/A'}`
3. `{metrics['top_signals_at_detection'][2] if len(metrics['top_signals_at_detection']) > 2 else 'N/A'}`

---

## Artifact Manifest


- Telemetry Archive: `flare_history.jsonl.zst`
- Extracted Feature Dataset: `dataset.csv`
- Model Checkpoint: `model.pt`
- Calibration Metadata: `model_meta.json`
- Quantitative Metrics: `evaluation_metrics.json`
- Multi-Panel Visualization: `evaluation_plot.png`
"""
    report_path.write_text(report_md, encoding="utf-8")
    print(f"  ✓ Executive report generated: {report_path.name}")
    return report_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", default=str(FLARE_DIR), help="Directory to save all output artifacts")
    parser.add_argument("--capture", action="store_true", help="Re-run physical Soft-PLC simulation capture from scratch")
    parser.add_argument("--epochs", type=int, default=40, help="Training epochs for WindowAutoencoder (default: 40)")
    parser.add_argument("--batch-size", type=int, default=32, help="Mini-batch size (default: 32)")
    parser.add_argument("--lr", type=float, default=0.001, help="Adam learning rate (default: 0.001)")
    parser.add_argument("--seed", type=int, default=42, help="Fixed random seed (default: 42)")
    parser.add_argument("--device", default=None, help="Device to use ('cpu', 'cuda', 'mps', or None for auto)")
    parser.add_argument("--no-plot", action="store_true", help="Skip matplotlib diagnostic plot generation")
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Create subdirectories for datasets and models
    dataset_dir = output_dir / "dataset"
    dataset_dir.mkdir(parents=True, exist_ok=True)
    model_dir = output_dir / "model"
    model_dir.mkdir(parents=True, exist_ok=True)

    config_path = getConfigPath(output_dir)
    if not config_path.is_file() and BASE_CONFIG_PATH.is_file():
        shutil.copy2(BASE_CONFIG_PATH, config_path)
    config = loadConfig(config_path) if config_path.is_file() else loadConfig(BASE_CONFIG_PATH)

    device = resolveDevice(args.device)
    hw_info = getHardwareInfo()

    printBanner(output_dir, device, hw_info)

    t0 = time.time()
    # Stage 1
    archive_path, meta_path = stage1Ingestion(output_dir, run_capture=args.capture)
    # Stage 2
    feature_df, timestamps, splits = stage2Dataset(archive_path, output_dir, config)
    # Stage 3
    detector = stage3TrainAndCalibrate(
        splits=splits,
        output_dir=output_dir,
        config=config,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        seed=args.seed,
        device=device,
    )
    # Stage 4
    metrics = stage4EvaluateAndPlot(
        detector=detector,
        splits=splits,
        output_dir=output_dir,
        config=config,
        make_plot=not args.no_plot,
    )
    # Stage 5
    stage5Report(metrics, output_dir)

    elapsed = time.time() - t0

    # Print Final Summary Table
    print("\n" + "=" * 80)
    print("                      ORCHESTRATION PIPELINE COMPLETE")
    print("=" * 80)
    print(f"  Total Wall Time:                 {elapsed:.2f}s")
    print(f"  Detection Delay:                 {metrics['detection_delay_hours']:.1f} hours")
    print(f"  ESD Lead Time:                   {metrics['lead_time_before_esd_hours']:.1f} hours before trip")
    print(f"  Abnormal False Alarm Rate:       {metrics['false_alarm_rate_abnormal_only'] * 100:.2f}%")
    print(f"  Primary Fault Dimension:         {metrics.get('primary_dimension', 'N/A')}")
    print(f"  Output Artifacts Stored In:      {output_dir}")
    print("=" * 80 + "\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
