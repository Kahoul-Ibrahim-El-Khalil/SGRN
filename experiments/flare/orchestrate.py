#!/usr/bin/env python3
"""experiments/flare/orchestrate.py — End-to-end pipeline orchestrator for the Flare Blocked experiment.

Orchestrates the entire research lifecycle with rich tqdm progress visualization:
  [1/5] Process Telemetry & WAL Ingestion (flare_history.jsonl.zst)
  [2/5] SCL Schema Parsing & Sliding Window Extraction (dataset.csv)
  [3/5] Manifold Autoencoder Training & Split-Conformal Calibration (model.pt, model_meta.json)
  [4/5] 742-Hour Timeline Evaluation & Dimensional Attribution (evaluation_plot.png, evaluation_metrics.json)
  [5/5] Executive Diagnostic Report Generation (report.md)

Usage:
   # --output-dir is REQUIRED — artifacts never silently land in CWD/source tree
   python3 experiments/flare/orchestrate.py --output-dir scratch/flare_run
   python3 experiments/flare/orchestrate.py --output-dir scratch/flare_run --config experiments/flare/config.yaml
   python3 experiments/flare/orchestrate.py --output-dir scratch/flare_run --epochs 50   # YAML overrides only when flagged
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

from config_loader import loadConfig, getConfigPath, getWindowRows

from sgrn.ml import (
    Scaler,
    configureModelFromSchema,
    createSlidingWindows,
    getHardwareInfo,
    resolveDevice,
    trainManifoldDetector,
)
from sgrn.ml.manifold import ProcessManifoldDetector
from evaluate import runEvaluation
from evaluation_common import configureSeeds

import torch

FLARE_DIR = Path(__file__).resolve().parent
BASE_CONFIG_PATH = FLARE_DIR / "config.json"
BASE_META_PATH = FLARE_DIR / "meta.json"
BASE_ARCHIVE_PATH = FLARE_DIR / "flare_history.jsonl.zst"
SCL_SCHEMA_PATH = ROOT / "sgrn/lib/gateway/simulations/gas_processing/schema.scl"


def _fmtHours(value: Optional[float]) -> str:
    """None-safe formatting for delay/lead-time values."""
    return f"{value:.1f} hours" if value is not None and np.isfinite(value) else "n/a (no detection)"


def resolveConfig(srcConfig: Optional[str], output_dir: Path) -> Tuple[Path, dict]:
    """Resolve the active config file and copy it into the run directory.

    Priority:
      1. ``--config PATH`` (YAML or JSON, copied verbatim into output_dir)
      2. FLARE_DIR preferred config (config.yaml > config.json via getConfigPath)

    The resolved file is copied into ``output_dir`` so every run directory is
    self-describing and reproducible, and the copy is the file that is loaded
    — no silent fallback to a stale sibling format.
    """
    if srcConfig:
        src = Path(srcConfig).resolve()
        if not src.is_file():
            raise FileNotFoundError(f"--config file not found: {src}")
        chosen = src
    else:
        chosen = getConfigPath(FLARE_DIR)  # prefers config.yaml
        if not chosen.is_file():
            raise FileNotFoundError(
                f"No config.yaml or config.json found in {FLARE_DIR}; "
                "pass --config PATH explicitly.")

    dest = output_dir / chosen.name
    if srcConfig or dest.resolve() != chosen.resolve():
        shutil.copy2(chosen, dest)
    config = loadConfig(dest)
    return dest, config


def mergeCliOverrides(config: dict, args: argparse.Namespace) -> dict:
    """Apply CLI hyperparameters ONLY when explicitly passed.

    YAML config values are authoritative by default; a bare
    ``--epochs``-style default previously stomped the YAML value (40 vs 50),
    making the config effectively dead. Here the CLI wins only when the
    user actually typed the flag.
    """
    if args.epochs is not None:
        config.setdefault("model", {})["epochs"] = args.epochs
    if args.batch_size is not None:
        config.setdefault("model", {})["batch_size"] = args.batch_size
    if args.lr is not None:
        config.setdefault("model", {})["learning_rate"] = args.lr
    if args.seed is not None:
        config.setdefault("dataset", {})["random_seed"] = args.seed
    return config


def effectiveHyperParams(config: dict) -> Dict[str, Any]:
    """Single source of truth for seed/epochs/... actually used by training."""
    model_cfg = config.get("model", {})
    return {
        "epochs": int(model_cfg.get("epochs", 50)),
        "batch_size": int(model_cfg.get("batch_size", 32)),
        "lr": float(model_cfg.get("learning_rate", 0.001)),
        "seed": int(config.get("dataset", {}).get("random_seed", 42)),
        "calibration_stride": config.get("dataset", {}).get("calibration_stride"),
        "alpha": float(model_cfg.get("alpha", 0.05)),
        "k_multiplier": float(model_cfg.get("k_multiplier", 1.4)),
    }


def printBanner(output_dir: Path, device: torch.device, hw_info: dict, config_path: Path) -> None:
    print("\n" + "=" * 80)
    print("      SGRN CYBER-PHYSICAL PLANT SUPERVISION — PIPELINE ORCHESTRATOR")
    print("=" * 80)
    print(f"  Target Output Dir:  {output_dir}")
    print(f"  Active Compute:     {device} ({hw_info['platform']} - {hw_info['backend']})")
    print(f"  Active Config:      {config_path}")
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


def stage1Ingestion(output_dir: Path, run_capture: bool, config_path: Path, local_capture: bool = False) -> Tuple[Path, Path]:
    """Ensure WAL archive and simulation metadata exist in output_dir."""
    print("▶ [1/5] Process Telemetry & WAL Ingestion")
    archive_out = output_dir / "flare_history.jsonl.zst"
    meta_out = output_dir / "meta.json"

    if run_capture:
        print("  Generating WAL via s7shell soft-PLC (Persistence — in-process capture)...")
        capture_script = FLARE_DIR / "run_capture.py"
        cmd = [sys.executable, str(capture_script),
               "--out", str(output_dir), "--config", str(config_path)]
        if local_capture:
            cmd.append("--local")
        pbar = tqdm(total=100, desc="  Capturing Soft-PLC Telemetry", bar_format="{l_bar}{bar:30}{r_bar}")
        res = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
        pbar.update(100)
        pbar.close()
        if res.returncode != 0:
            raise RuntimeError(f"run_capture.py failed:\n{res.stdout}\n{res.stderr}")
        if not archive_out.is_file():
            raise RuntimeError(
                "run_capture.py succeeded but no archive landed in output dir; "
                "run run_capture.py --verbose to diagnose.")
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


from dataset import getSplitsAndWindows


def stage2Dataset(archive_path: Path, output_dir: Path, config: dict, exportCsv: Path | None = None) -> Tuple[np.ndarray, pd.Series, dict]:
    """Stream the WAL archive into the numeric cache and extract chronological sliding windows."""
    print("\n▶ [2/5] SCL Schema Parsing & Streaming Window Extraction")
    cache_prefix = output_dir / "dataset" / "window_cache" / "window_cache"

    meta_path = output_dir / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else json.loads(BASE_META_PATH.read_text(encoding="utf-8"))

    splits = getSplitsAndWindows(archive_path=archive_path, cache_prefix=cache_prefix,
                                 export_csv=exportCsv, config=config, meta=meta)

    feature_matrix = splits["timeline_values"]
    timestamps = splits["timestamps"]

    X_train = splits["X_train"]
    X_cal = splits["X_cal"]
    X_test = splits["X_test"]

    window_rows = X_train.shape[1] if X_train.ndim >= 2 else 1
    print(f"  ✓ Windows extracted: Train={X_train.shape[0]} | Cal={X_cal.shape[0]} | Fault Test={X_test.shape[0]}")
    print(f"  ✓ Features: {len(splits['features'])} process variables ({window_rows}-sample windows)")

    return feature_matrix, timestamps, splits


def stage3TrainAndCalibrate(
    splits: dict,
    output_dir: Path,
    config: dict,
    hyper: Dict[str, Any],
    device: torch.device,
) -> ProcessManifoldDetector:
    """Train WindowAutoencoder and compute split-conformal calibration thresholds.

    All hyperparameters (epochs, batch_size, lr, alpha, k_multiplier,
    calibration_stride, seed) come from the resolved config — the same values
    surface in the banner/logs so runs are auditable.
    """
    print("\n▶ [3/5] Manifold Autoencoder Training & Split-Conformal Calibration")

    seed = int(hyper["seed"])
    configureSeeds(seed)  # random + numpy + torch, from config (reproducibility)

    X_train = splits["X_train"]
    X_cal = splits["X_cal"]
    scaler = splits["scaler"]
    features = splits["features"]
    window_size = getWindowRows(config)

    # Physical dimensions/subsystems from the authoritative schema parse.
    schema_cfg = configureModelFromSchema(SCL_SCHEMA_PATH, windowSize=window_size)
    schema_dims = schema_cfg.get("featureDimensions", {})
    schema_subs = schema_cfg.get("featureSubsystems", {})
    feature_dims = {f: schema_dims.get(f, "General") for f in features}
    feature_subs = {f: schema_subs.get(f, f.split(".", 1)[0] if "." in f else "General")
                    for f in features}

    model_cfg = dict(config.get("model", {}))
    print(f"  Training config: epochs={hyper['epochs']} batch_size={hyper['batch_size']} "
          f"lr={hyper['lr']} alpha={hyper['alpha']} k_multiplier={hyper['k_multiplier']} "
          f"seed={seed} calibration_stride={hyper['calibration_stride'] or 'auto'}")

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
        calibrationStride=hyper["calibration_stride"] if hyper["calibration_stride"] else None,
    )

    detector.save(output_dir / "model")
    print(f"  ✓ Split-Conformal Calibration complete:")
    print(f"    - Bounded Threshold q_alpha (1 - alpha = {1 - detector.alpha:.2f}): {detector.q_alpha:.4f}")
    print(f"    - Upper Safety Bound q_upper (k={detector.k_multiplier:.1f}, heuristic): {detector.q_upper:.4f}")

    # Finite-sample conformal guard: with alpha coverage 1/alpha calibration
    # scores are needed for a NON-INF quantile. Too few → detector can never
    # flag anything; surface it loudly with the config knobs that fix it.
    if not np.isfinite(detector.q_alpha):
        alpha = float(detector.alpha)
        print("  ⚠ WARNING: q_alpha = INF — the detector can never warn (never-flag regime).")
        print(f"    The calibration split yields too few effective windows (need >= {int(np.ceil(1.0 / alpha))} for alpha={alpha}).")
        print("    Fix in config.yaml:")
        print("      dataset.calibration_stride: 1   (or delete the key; currently thins to the window length)")
        print("      dataset.calibration_ratio: larger (more pre-fault rows into calibration)")
        print("      model.alpha: larger (weaker guarantee, e.g. 0.10 → n >= 10)")

    print(f"  ✓ Model weights & metadata saved to {output_dir / 'model'}")
    return detector



def stage4EvaluateAndPlot(
    detector: ProcessManifoldDetector,
    splits: dict,
    output_dir: Path,
    config: dict,
    config_path: Path,
    make_plot: bool = True,
) -> dict:
    """Evaluate the timeline via the shared single code path (evaluate.py).

    Held-out-normal FAR (+ Clopper–Pearson CI on non-overlapping windows),
    calibration-split rates under explicitly circular keys, calibrated
    single-signal baselines, delay/lead time, attribution and provenance are
    produced by ``evaluate.evaluateDetector`` — orchestrate and the
    standalone ``flare evaluate`` command can no longer diverge.
    """
    print("\n▶ [4/5] Full-Timeline Evaluation, Held-Out FAR, Baselines & Attribution")
    return runEvaluation(
        save_plot=make_plot,
        model_dir=output_dir / "model",
        output_dir=output_dir,
        config_path=config_path,
        archive_path=(splits.get("archive_path")
                      or (output_dir / "flare_history.jsonl.zst")),
        meta_path=(output_dir / "meta.json" if (output_dir / "meta.json").is_file()
                   else BASE_META_PATH),
    )


def stage5Report(metrics: dict, output_dir: Path) -> Path:
    """Generate Markdown executive report (claims follow the measured metrics)."""
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

    heldout = metrics.get("false_alarm_rate_heldout", {}) or {}
    heldout_indep = metrics.get("false_alarm_rate_heldout_non_overlapping", {}) or {}
    far = metrics.get("false_alarm_rate_abnormal_only")
    delay = metrics.get("detection_delay_hours")
    lead = metrics.get("lead_time_before_esd_hours")
    primary_feat = metrics.get("primary_dimension", "N/A")
    primary_sub = metrics.get("primary_subsystem", "SafetySystems")
    flare_top3 = bool(metrics.get("flare_header_press_in_top3"))
    feed_top3 = bool(metrics.get("feed_pressure_in_top3"))
    detected = metrics.get("first_abnormal_iso") is not None
    never_flag = bool(metrics.get("never_flag_regime"))
    ci = heldout_indep.get("clopper_pearson_95_abnormal_only") or [0.0, 1.0]

    baseline_rows = ""
    for name, b in (metrics.get("baselines") or {}).items():
        if not isinstance(b, dict) or "detection_delay_hours" not in b:
            continue
        baseline_rows += (
            f"| {name} (= train μ + {b['k']:.2f}σ) | "
            f"{b['heldout']['rate_abnormal_only'] * 100:.2f}% "
            f"({b['heldout']['n_flagged_abnormal']}/{b['heldout']['n_windows']}) | "
            f"{_hours(b['detection_delay_hours'])} | "
            f"{_hours(b['lead_time_before_esd_hours'])} |\n")

    far_summary = "n/a" if far is None else f"{far * 100:.2f}%"
    if never_flag:
        far_summary += " (never-flag regime: q_alpha infinite, see limitations)"

    report_md = f"""# SGRN Autonomous Supervision — Flare Blocked Anomaly Detection Report

**Generated:** {datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")}  
**Target Output Directory:** `{output_dir}`

---

## Executive Summary

The split-conformal manifold detector was evaluated across the full archived plant simulation. The false-alarm rate is measured on **held-out-normal windows** (never on the calibration split used to set q_alpha — that measurement is circular by construction and lives under `calibration_rate_*` in `evaluation_metrics.json`). Detection delay is the time from fault onset to the first ABNORMAL window; lead time is the remaining time before the simulated ESD trip. Single-signal baselines (threshold = train mean + k·std on the raw flare-header pressure level and slope, with k chosen on the calibration split only) are reported alongside for context.

| Metric | Result | Target Benchmark | Status |
|:---|:---:|:---:|:---:|
| **Held-Out-Normal FAR (Abnormal Only)** | **{far_summary}** | ≤ 5.0% conformal budget | {_status(far is not None and far <= 0.05)} |
| **Held-Out-Normal FAR (Uncertain or Abnormal)** | **{"n/a" if heldout.get("rate_total") is None else f"{heldout['rate_total'] * 100:.2f}%"}** (autoencoder) | ≤ alpha + margin | informational |
| Independent windows (stride = window size) | {heldout_indep.get("n_independent_windows", "n/a")} | flagged: {heldout_indep.get("n_flagged_abnormal", "n/a")} | FAR 95% CI ≈ [{ci[0] * 100:.2f}%, {ci[1] * 100:.2f}%] |
| **Detection Delay** | **{_hours(delay)}** | ≤ 2.0 hours | {_status(detected and delay is not None and delay <= 2.0)} |
| **Lead Time Before Emergency Shutdown (ESD)** | **{_hours(lead)}** | ≥ 12.0 hours | {_status(lead is not None and lead >= 12.0)} |
| **Primary Drifting Feature Dimension** | **`{primary_feat}`** | `SafetySystems.flare_header_press` | {_status(primary_feat == "SafetySystems.flare_header_press")} |
| **Primary Subsystem / Device** | **`{primary_sub}`** | `SafetySystems` | {_status(primary_sub == "SafetySystems")} |
| **Flare Header in Top-3 Deviations** | **{flare_top3}** | True | {_status(flare_top3)} |
| **Feed Pressure in Top-3 Deviations** | **{feed_top3}** | True | {_status(feed_top3)} |

**Detector vs baselines:** {metrics.get("detector_vs_baselines", {}).get("summary", "n/a")}

## Baselines (same data, calibrated on the calibration split only)

| Baseline | Held-out FAR (abnormal only) | Detection delay | Lead time before ESD |
|:---|:---:|:---:|:---:|
{baseline_rows if baseline_rows else "| (none computed) | | | |"}

---

## Split-Conformal Calibration Thresholds

- **Calibration Sample Size (n_cal, stride-1 windows)**: {metrics.get('calibration_windows', 'n/a')} raw / {metrics.get('n_calibration_effective_windows', 'n/a')} effective (thinned at stride {metrics.get('calibration_stride', 'n/a')})
- **Conformal Threshold (q_alpha)**: `{metrics['q_alpha']:.4f}`
- **Upper Safety Bound (q_upper, heuristic multiplier, NOT a calibrated level)**: `{metrics['q_upper']:.4f}`
- Calibration-split exceedance rates (`calibration_rate_*` in JSON) are **circular**: the calibration windows define q_alpha. They are recorded for provenance only.

---

## Top Feature Dimension Deviations at Detection (t = {metrics.get('first_abnormal_iso') or 'no detection'})

| Feature Dimension | Residual MSE | Contribution Percentage | Subsystem / Device |
|:---|:---:|:---:|:---:|
{dim_breakdown_rows}

---

## Limitations

- A single fault scenario (`FLARE_BLOCKED`, fault_code 9) on our own simulator (`simulation.as`) — the only available data source. No real plant data, no cross-scenario validation.
- The held-out normal data comes from a chronological split of the same pre-fault run (row-to-row autocorrelation crosses the split boundary); the simulator's noise LCG is not seed-controlled, so a re-captured "normal" run replays the same trajectory rather than providing an independent realization.
- The fault is a LINEAR RAMP on `flare_header_press` (plus hydraulic backpressure coupling) — the detector attributes the anomaly to that same ramp; attribution is correlational ranking, not causal proof.
- Simulated telemetry is persisted on change only, so rows are irregularly spaced; sliding "12-hour" windows cover variable physical durations.
- Sliding windows with stride 1 are strongly autocorrelated: the vanilla conformal coverage statement does not formally hold; q_upper = k·q_alpha is a heuristic alerting margin, not a second conformal quantile.
- Small effective sample everywhere (single 31-day run); all rates carry tiny-sample CIs, and a different capture could legitimately produce different numbers.

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
    parser.add_argument("--output-dir", required=True,
                        help="REQUIRED. Directory to save all output artifacts "
                             "(dataset/, model/, metrics, plot, report). "
                             "No sentinel default: an explicit destination avoids "
                             "silently polluting the CWD or source tree.")
    parser.add_argument("--config", default=None,
                        help="Config file (YAML or JSON). Default: experiments/flare/config.yaml "
                             "(falls back to config.json). The file is copied into --output-dir "
                             "and the copy governs the run.")
    parser.add_argument("--capture", action="store_true", help="Re-run physical Soft-PLC simulation capture from scratch")
    parser.add_argument("--local", action="store_true",
                        help="Capture in-process via the s7shell Persistence binding (binary WAL, no gateway process)")
    parser.add_argument("--epochs", type=int, default=None,
                        help="Override config.yaml model.epochs (default: take value from YAML)")
    parser.add_argument("--batch-size", type=int, default=None,
                        help="Override config.yaml model.batch_size (default: take value from YAML)")
    parser.add_argument("--lr", type=float, default=None,
                        help="Override config.yaml model.learning_rate (default: take value from YAML)")
    parser.add_argument("--seed", type=int, default=None,
                        help="Override config.yaml dataset.random_seed (default: take value from YAML)")
    parser.add_argument("--device", default=None, help="Device to use ('cpu', 'cuda', 'mps', or None for auto)")
    parser.add_argument("--no-plot", action="store_true", help="Skip matplotlib diagnostic plot generation")
    parser.add_argument("--export-csv", action="store_true",
                        help="Also emit dataset.csv during stage 2 (debug/diff aid; off by default)")
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    # Resolve + stage the active config into the run dir (YAML preferred).
    config_path, config = resolveConfig(args.config, output_dir)
    config = mergeCliOverrides(config, args)
    hyper = effectiveHyperParams(config)

    # Create subdirectories for datasets and models
    dataset_dir = output_dir / "dataset"
    dataset_dir.mkdir(parents=True, exist_ok=True)
    model_dir = output_dir / "model"
    model_dir.mkdir(parents=True, exist_ok=True)

    device = resolveDevice(args.device)
    hw_info = getHardwareInfo()

    printBanner(output_dir, device, hw_info, config_path)

    t0 = time.time()
    # Stage 1
    archive_path, meta_path = stage1Ingestion(output_dir, run_capture=args.capture, config_path=config_path,
                                              local_capture=args.local)
    # Stage 2
    export_csv = (output_dir / "dataset" / "dataset.csv") if getattr(args, "export_csv", False) else None
    feature_df, timestamps, splits = stage2Dataset(archive_path, output_dir, config, exportCsv=export_csv)
    # Stage 3
    detector = stage3TrainAndCalibrate(
        splits=splits,
        output_dir=output_dir,
        config=config,
        hyper=hyper,
        device=device,
    )
    # Stage 4
    metrics = stage4EvaluateAndPlot(
        detector=detector,
        splits=splits,
        output_dir=output_dir,
        config=config,
        config_path=config_path,
        make_plot=not args.no_plot,
    )
    # Stage 5
    stage5Report(metrics, output_dir)

    elapsed = time.time() - t0

    # Print Final Summary Table
    heldout = (metrics.get("false_alarm_rate_heldout") or {})
    print("\n" + "=" * 80)
    print("                      ORCHESTRATION PIPELINE COMPLETE")
    print("=" * 80)
    print(f"  Total Wall Time:                 {elapsed:.2f}s")
    print(f"  Detection Delay:                 {_fmtHours(metrics['detection_delay_hours'])}")
    print(f"  ESD Lead Time:                   {_fmtHours(metrics['lead_time_before_esd_hours'])} before trip")
    print(f"  Abnormal FAR (held-out normal):  {metrics['false_alarm_rate_abnormal_only'] * 100:.2f}%"
          f" ({heldout.get('n_flagged_abnormal')}/{heldout.get('n_windows')} windows,"
          f" {metrics.get('false_alarm_rate_heldout_non_overlapping', {}).get('n_independent_windows')} independent)")
    print(f"  Primary Fault Dimension:         {metrics.get('primary_dimension', 'N/A')}")
    print(f"  Baseline Verdict:                {metrics.get('detector_vs_baselines', {}).get('summary', 'n/a')}")
    print(f"  Output Artifacts Stored In:      {output_dir}")
    print("=" * 80 + "\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
