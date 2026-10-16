#!/usr/bin/env python3
"""Evaluation script for the Flare Blocked anomaly detection prototype.

Calculates and prints:
  - False alarm rate on held-out normal calibration portion
  - Detection delay (first abnormal window minus fault_onset_ts)
  - Lead time before ESD (esd_ts minus first abnormal window)
  - Attribution check: whether flare_header_press or feed_pressure is in top-3 at detection
  - Generates evaluation_plot.png (score vs time with thresholds, fault onset, and ESD markers)

One command:
  python experiments/flare/evaluate.py
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

from config_loader import loadConfig, getConfigPath
from dataset import loadDatasetDf, ensureDatasetCsv, resolveModelDir, SCL_SCHEMA
from model import FlareDetector


FLARE_DIR = Path(__file__).parent
CONFIG_PATH = getConfigPath(FLARE_DIR)
META_PATH = FLARE_DIR / "meta.json"
ARCHIVE_PATH = FLARE_DIR / "flare_history.jsonl.zst"
CSV_PATH = FLARE_DIR / "dataset.csv"
DEFAULT_MODEL_DIR = FLARE_DIR / "model"


def epochMsUtc(dt) -> int:
    """Naive datetimes are UTC instants (gateway frame clock is ms UTC epoch).

    Never use naive ``.timestamp()`` here: it interprets wall time in the
    HOST timezone and skews delays by the UTC offset on non-UTC machines.
    """
    import calendar

    tt = pd.to_datetime(dt)
    if getattr(tt, "tzinfo", None) is not None:
        tt = tt.tz_convert("UTC").tz_localize(None)
    return calendar.timegm(tt.timetuple()) * 1000 + int(tt.microsecond / 1000)


def parseEvaluationTimestamps(meta: dict):
    """Extract and validate (fault_onset, esd|None) datetimes from experiment metadata.

    Raises ValueError with an actionable message on missing or unparseable
    timestamps instead of a bare KeyError mid-evaluation.
    """
    for key in ("fault_onset_iso", "fault_onset_ts"):
        if key not in meta:
            raise ValueError(f"Experiment metadata is missing required key '{key}'")
    try:
        _dt = pd.to_datetime(meta["fault_onset_iso"])
        fault_onset_dt = _dt.tz_convert(None) if getattr(_dt, "tzinfo", None) else _dt.tz_localize(None)
    except Exception as e:
        raise ValueError(f"Invalid fault_onset_iso {meta['fault_onset_iso']!r}: {e}") from e
    try:
        fault_onset_ts = int(meta["fault_onset_ts"])
    except (TypeError, ValueError) as e:
        raise ValueError(f"Invalid fault_onset_ts {meta['fault_onset_ts']!r}: {e}") from e
    esd_ts = meta.get("esd_ts")
    if esd_ts is not None:
        try:
            esd_ts = int(esd_ts)
        except (TypeError, ValueError) as e:
            raise ValueError(f"Invalid esd_ts {meta.get('esd_ts')!r}: {e}") from e
    return fault_onset_dt, fault_onset_ts, esd_ts


def computeEvaluationMetrics(all_scores, all_states, all_times, all_results,
                             fault_onset_dt, fault_onset_ts, esd_ts=None,
                             esd_iso=None, fault_onset_iso=None,
                             q_alpha=None, q_upper=None):
    """Pure metric computation over one scored sweep.

    Zero detections is a valid outcome (metrics carry ``None`` for undefined
    delays — serialized as ``null`` — while every other metric is preserved),
    as are empty inputs (all counts zero, delays ``None``). No plotting or
    file IO happens here, so this is directly unit-testable.
    """
    all_scores = list(all_scores)
    all_states = list(all_states)
    all_times = list(all_times)
    all_results = list(all_results)

    first_abnormal_idx = None
    first_abnormal_time = None
    first_abnormal_res = None
    for idx, (res, t) in enumerate(zip(all_results, all_times)):
        if t >= fault_onset_dt and res.get("state") == "abnormal":
            first_abnormal_idx = idx
            first_abnormal_time = t
            first_abnormal_res = res
            break

    first_abnormal_epoch_ms = (
        epochMsUtc(first_abnormal_time) if first_abnormal_time is not None else None
    )
    if first_abnormal_epoch_ms is not None:
        detection_delay_hours = (first_abnormal_epoch_ms - fault_onset_ts) / 3600000.0
    else:
        detection_delay_hours = None

    if esd_ts is not None and first_abnormal_epoch_ms is not None:
        lead_time_hours = (esd_ts - first_abnormal_epoch_ms) / 3600000.0
    else:
        lead_time_hours = None

    # Attribution check at detection: candidate ranking, not root cause.
    top_signals_at_det = ([s["signal"] for s in first_abnormal_res.get("top_signals", [])
                           if isinstance(s, dict) and "signal" in s]
                          if first_abnormal_res else [])
    flare_in_top3 = any("flare_header_press" in s for s in top_signals_at_det)
    feed_in_top3 = any("feed_pressure" in s for s in top_signals_at_det)

    return {
        "n_windows": len(all_scores),
        "fault_onset_iso": fault_onset_iso,
        "first_abnormal_iso": first_abnormal_time.isoformat() if first_abnormal_time else None,
        "detection_delay_hours": detection_delay_hours,
        "esd_iso": esd_iso,
        "lead_time_before_esd_hours": lead_time_hours,
        "top_signals_at_detection": top_signals_at_det,
        "flare_header_press_in_top3": flare_in_top3,
        "feed_pressure_in_top3": feed_in_top3,
        "primary_fault_in_top3": flare_in_top3 or feed_in_top3,
        "q_alpha": q_alpha,
        "q_upper": q_upper,
    }


def runEvaluation(save_plot: bool = True, model_dir: Path = DEFAULT_MODEL_DIR,
                  output_dir: Path = FLARE_DIR) -> dict:
    config = loadConfig(CONFIG_PATH)
    meta = json.loads(META_PATH.read_text(encoding="utf-8"))
    try:
        fault_onset_dt, fault_onset_ts, esd_ts = parseEvaluationTimestamps(meta)
    except ValueError as e:
        print(f"[evaluate] ERROR: {e}")
        raise SystemExit(f"Invalid experiment metadata: {e}")

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    model_dir = resolveModelDir(model_dir)

    ensureDatasetCsv(ARCHIVE_PATH, CSV_PATH)
    features = config["features"]
    feature_df, sim_timestamps = loadDatasetDf(CSV_PATH, features)

    detector = FlareDetector.load(model_dir, config)
    window_size = detector.window_size

    # Split indices (timestamps already validated above)
    sim_dt_naive = sim_timestamps.dt.tz_localize(None)

    pre_fault_mask = sim_dt_naive < fault_onset_dt
    fault_mask = sim_dt_naive >= fault_onset_dt

    pre_indices = np.where(pre_fault_mask)[0]
    fault_indices = np.where(fault_mask)[0]

    n_pre = len(pre_indices)
    train_ratio = config.get("dataset", {}).get("train_ratio", 0.70)
    n_train = int(n_pre * train_ratio)

    cal_indices = pre_indices[n_train:]

    raw_values = feature_df.to_numpy(dtype=np.float32)

    # 1. Evaluate Calibration Windows (False Alarm Rate)
    cal_scores = []
    cal_states = []
    for i in range(len(cal_indices) - window_size + 1):
        idx_slice = cal_indices[i : i + window_size]
        win = raw_values[idx_slice]
        res = detector.detect(win)
        cal_scores.append(res["score"])
        cal_states.append(res["state"])

    cal_count = len(cal_states)
    cal_uncertain = sum(1 for s in cal_states if s == "uncertain")
    cal_abnormal = sum(1 for s in cal_states if s == "abnormal")
    far_total = (cal_uncertain + cal_abnormal) / max(1, cal_count)
    far_abnormal_only = cal_abnormal / max(1, cal_count)

    # 2. Evaluate Full Time Series (Sliding Window across sequence)
    all_scores = []
    all_states = []
    all_results = []
    all_times = []

    for i in range(len(raw_values) - window_size + 1):
        win = raw_values[i : i + window_size]
        res = detector.detect(win)
        ts = sim_timestamps.iloc[i + window_size - 1]
        all_scores.append(res["score"])
        all_states.append(res["state"])
        all_results.append(res)
        all_times.append(ts)

    all_times_series = pd.Series(all_times)

    # 3. Fault Evaluation (Detection Delay & ESD Lead Time)
    fault_indices = np.where(fault_mask)[0]
    if len(fault_indices) == 0:
        print("[evaluate] WARNING: no rows at/after fault onset — "
              "test and delay metrics will be empty, not failed.")

    metrics = computeEvaluationMetrics(
        all_scores, all_states, all_times, all_results,
        fault_onset_dt, fault_onset_ts, esd_ts,
        esd_iso=meta.get("esd_iso"), fault_onset_iso=meta.get("fault_onset_iso"),
        q_alpha=detector.q_alpha, q_upper=detector.q_upper,
    )
    metrics.update({
        "calibration_windows": cal_count,
        "false_alarm_rate_total": float(far_total),
        "false_alarm_rate_abnormal_only": float(far_abnormal_only),
    })
    first_abnormal_time = (pd.to_datetime(metrics["first_abnormal_iso"])
                           if metrics["first_abnormal_iso"] else None)
    detection_delay_hours = metrics["detection_delay_hours"]
    lead_time_hours = metrics["lead_time_before_esd_hours"]
    top_signals_at_det = metrics["top_signals_at_detection"]
    flare_in_top3 = metrics["flare_header_press_in_top3"]
    feed_in_top3 = metrics["feed_pressure_in_top3"]
    primary_fault_in_top3 = metrics["primary_fault_in_top3"]

    # Print Metrics Table
    print("\n" + "=" * 80)
    print("                    EVALUATION METRICS SUMMARY")
    print("=" * 80)
    print(f"  Calibration Windows:              {cal_count}")
    print(f"  False Alarm Rate (Uncertain/Abn): {far_total * 100:.2f}%")
    print(f"  False Alarm Rate (Abnormal only): {far_abnormal_only * 100:.2f}%")
    print("-" * 80)
    print(f"  Fault Onset:                      {meta['fault_onset_iso']} (Day 31, Hour 3)")
    if first_abnormal_time is not None and detection_delay_hours is not None:
        print(f"  First Abnormal Detection:         {first_abnormal_time} (Delay: {detection_delay_hours:.1f} sim hours)")
    else:
        print("  First Abnormal Detection:         None (no abnormal window at/after onset)")
    if esd_ts:
        print(f"  ESD Trip Event:                   {meta['esd_iso']} (Day 31, Hour 22)")
        if lead_time_hours is not None:
            print(f"  Lead Time Before ESD:             {lead_time_hours:.1f} sim hours")
        else:
            print("  Lead Time Before ESD:             None (no detection before ESD)")
    else:
        print("  ESD Trip Event:                   None")
    print("-" * 80)
    print(f"  Top-3 Signals at Detection:       {top_signals_at_det}")
    print(f"  Flare Header in Top-3:            {flare_in_top3}")
    print(f"  Feed Pressure in Top-3:           {feed_in_top3}")
    print(f"  Primary Fault Signals In Top-3:   {primary_fault_in_top3} (ranking, not root cause)")
    print("=" * 80 + "\n")

    # Save metrics JSON
    metrics_path = output_dir / "evaluation_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(f"[evaluate] Metrics saved to {metrics_path}")

    # 4. Generate Plot
    if save_plot:
        if not all_scores:
            print("[evaluate] No scored windows; skipping plot.")
        else:
            plot_path = output_dir / "evaluation_plot.png"
            fig, ax = plt.subplots(figsize=(14, 6), dpi=150)

            # Elapsed simulated hours from start
            start_t = sim_timestamps.iloc[0]
            elapsed_hours = [(t - start_t).total_seconds() / 3600.0 for t in all_times]

            ax.plot(elapsed_hours, all_scores, label="Reconstruction Score (MSE)", color="#1f77b4", linewidth=1.5)

            # Thresholds
            ax.axhline(detector.q_alpha, color="#ff7f0e", linestyle="--", linewidth=1.2, label=f"q_alpha ({detector.q_alpha:.3f})")
            ax.axhline(detector.q_upper, color="#d62728", linestyle="--", linewidth=1.2, label=f"Upper Uncertain Bound ({detector.q_upper:.3f})")

            # Shaded background states
            max_y = min(150.0, max(all_scores) * 1.1)
            ax.set_ylim(-0.1, max_y)
            ax.axhspan(-0.1, detector.q_alpha, color="green", alpha=0.08, label="Normal Zone")
            ax.axhspan(detector.q_alpha, detector.q_upper, color="orange", alpha=0.10, label="Uncertain Zone")
            ax.axhspan(detector.q_upper, max_y, color="red", alpha=0.08, label="Abnormal Zone")

            # Markers
            fault_elapsed = (fault_onset_dt - start_t).total_seconds() / 3600.0
            ax.axvline(fault_elapsed, color="purple", linestyle="-.", linewidth=1.5, label=f"Fault Onset (t={fault_elapsed:.0f}h)")

            if esd_ts and first_abnormal_time:
                esd_dt = pd.to_datetime(meta["esd_iso"]).tz_localize(None)
                esd_elapsed = (esd_dt - start_t).total_seconds() / 3600.0
                ax.axvline(esd_elapsed, color="black", linestyle=":", linewidth=1.8, label=f"ESD Trip (t={esd_elapsed:.0f}h)")

                det_elapsed = (first_abnormal_time - start_t).total_seconds() / 3600.0
                ax.scatter([det_elapsed], [first_abnormal_res["score"]], color="red", marker="*", s=220, zorder=5, label=f"First Detection (t={det_elapsed:.0f}h)")

            ax.set_title("Flare Blocked Anomaly Detection — Autoencoder Reconstruction Score vs Time", fontsize=14, fontweight="bold", pad=12)
            ax.set_xlabel("Simulated Plant Operation (Hours)", fontsize=11)
            ax.set_ylabel("Reconstruction Error Score", fontsize=11)
            ax.grid(True, linestyle=":", alpha=0.6)
            ax.legend(loc="upper left", fontsize=9, framealpha=0.9)

            fig.tight_layout()
            fig.savefig(plot_path)
            plt.close(fig)
            print(f"[evaluate] Plot saved to {plot_path}")

    return metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-plot", action="store_true", help="Skip saving evaluation plot")
    parser.add_argument("--model-dir", default=str(DEFAULT_MODEL_DIR),
                        help="Directory holding model.pt + model_meta.json "
                             "(train_detector.py --save-dir <dir> writes <dir>/model/)")
    parser.add_argument("--output-dir", default=str(FLARE_DIR),
                        help="Directory for evaluation_metrics.json and evaluation_plot.png")
    args = parser.parse_args()

    runEvaluation(save_plot=not args.no_plot, model_dir=Path(args.model_dir),
                  output_dir=Path(args.output_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
