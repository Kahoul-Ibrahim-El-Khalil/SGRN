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

from config_loader import load_config, get_config_path
from dataset import load_dataset_df, ensure_dataset_csv, SCL_SCHEMA
from model import FlareDetector


FLARE_DIR = Path(__file__).parent
CONFIG_PATH = get_config_path(FLARE_DIR)
META_PATH = FLARE_DIR / "meta.json"
ARCHIVE_PATH = FLARE_DIR / "flare_history.jsonl.zst"
CSV_PATH = FLARE_DIR / "dataset.csv"


def run_evaluation(save_plot: bool = True) -> dict:
    config = load_config(CONFIG_PATH)
    meta = json.loads(META_PATH.read_text(encoding="utf-8"))

    ensure_dataset_csv(ARCHIVE_PATH, CSV_PATH)
    features = config["features"]
    feature_df, sim_timestamps = load_dataset_df(CSV_PATH, features)

    detector = FlareDetector.load(FLARE_DIR, config)
    window_size = detector.window_size

    # Split indices
    fault_onset_dt = pd.to_datetime(meta["fault_onset_iso"]).tz_localize(None)
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
    fault_start_idx = fault_indices[0]
    fault_onset_ts = meta["fault_onset_ts"]
    esd_ts = meta.get("esd_ts")

    # Find first abnormal detection in test sequence
    first_abnormal_idx = None
    first_abnormal_time = None
    first_abnormal_res = None

    for idx, (res, t) in enumerate(zip(all_results, all_times)):
        if t >= fault_onset_dt and res["state"] == "abnormal":
            first_abnormal_idx = idx
            first_abnormal_time = t
            first_abnormal_res = res
            break

    # Calculate detection delay and ESD lead time in simulated hours
    first_abnormal_epoch_ms = int(first_abnormal_time.timestamp() * 1000) if first_abnormal_time else None

    ms_per_hour = 3600 * 1000.0
    if first_abnormal_epoch_ms is not None:
        detection_delay_ms = first_abnormal_epoch_ms - fault_onset_ts
        detection_delay_hours = detection_delay_ms / ms_per_hour
    else:
        detection_delay_ms = None
        detection_delay_hours = None

    if esd_ts is not None and first_abnormal_epoch_ms is not None:
        lead_time_ms = esd_ts - first_abnormal_epoch_ms
        lead_time_hours = lead_time_ms / ms_per_hour
    else:
        lead_time_ms = None
        lead_time_hours = None

    # Attribution check: Is flare_header_press or feed_pressure in top-3 at detection?
    top_signals_at_det = [s["signal"] for s in first_abnormal_res["top_signals"]] if first_abnormal_res else []
    flare_in_top3 = any("flare_header_press" in s for s in top_signals_at_det)
    feed_in_top3 = any("feed_pressure" in s for s in top_signals_at_det)
    primary_fault_identified = flare_in_top3 or feed_in_top3

    metrics = {
        "calibration_windows": cal_count,
        "false_alarm_rate_total": float(far_total),
        "false_alarm_rate_abnormal_only": float(far_abnormal_only),
        "fault_onset_iso": meta["fault_onset_iso"],
        "first_abnormal_iso": first_abnormal_time.isoformat() if first_abnormal_time else None,
        "detection_delay_hours": float(detection_delay_hours) if detection_delay_hours is not None else None,
        "esd_iso": meta.get("esd_iso"),
        "lead_time_before_esd_hours": float(lead_time_hours) if lead_time_hours is not None else None,
        "top_signals_at_detection": top_signals_at_det,
        "flare_header_press_in_top3": flare_in_top3,
        "feed_pressure_in_top3": feed_in_top3,
        "primary_fault_identified": primary_fault_identified,
        "q_alpha": detector.q_alpha,
        "q_upper": detector.q_upper,
    }

    # Print Metrics Table
    print("\n" + "=" * 80)
    print("                    EVALUATION METRICS SUMMARY")
    print("=" * 80)
    print(f"  Calibration Windows:              {cal_count}")
    print(f"  False Alarm Rate (Uncertain/Abn): {far_total * 100:.2f}%")
    print(f"  False Alarm Rate (Abnormal only): {far_abnormal_only * 100:.2f}%")
    print("-" * 80)
    print(f"  Fault Onset:                      {meta['fault_onset_iso']} (Day 31, Hour 3)")
    print(f"  First Abnormal Detection:         {first_abnormal_time} (Delay: {detection_delay_hours:.1f} sim hours)")
    if esd_ts:
        print(f"  ESD Trip Event:                   {meta['esd_iso']} (Day 31, Hour 22)")
        print(f"  Lead Time Before ESD:             {lead_time_hours:.1f} sim hours")
    else:
        print("  ESD Trip Event:                   None")
    print("-" * 80)
    print(f"  Top-3 Signals at Detection:       {top_signals_at_det}")
    print(f"  Flare Header in Top-3:            {flare_in_top3}")
    print(f"  Feed Pressure in Top-3:           {feed_in_top3}")
    print(f"  Primary Fault Identified:         {primary_fault_identified}")
    print("=" * 80 + "\n")

    # Save metrics JSON
    metrics_path = FLARE_DIR / "evaluation_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(f"[evaluate] Metrics saved to {metrics_path}")

    # 4. Generate Plot
    if save_plot:
        plot_path = FLARE_DIR / "evaluation_plot.png"
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
    args = parser.parse_args()

    run_evaluation(save_plot=not args.no_plot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
