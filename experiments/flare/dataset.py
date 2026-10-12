#!/usr/bin/env python3
"""Dataset loader, chronological split, standardisation and sliding window generator.

Pipeline:
  1. Reconstruct signal state over time from archive / CSV.
  2. Select configured numeric process signals (excluding alarms, ESD, timestamps).
  3. Chronological split:
     - 70% of pre-fault -> train
     - 30% of pre-fault -> calibration
     - fault day -> test (never seen during train or calibration)
  4. Standardisation fitted strictly on train split.
  5. Sliding window generation.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Tuple

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[2]
SCL_SCHEMA = ROOT / "sgrn/lib/gateway/simulations/gas_processing/schema.scl"
CONFIG_PATH = Path(__file__).with_name("config.json")
META_PATH = Path(__file__).with_name("meta.json")


from sgrn.ml import Scaler



def find_sgrn_dataset_bin() -> Path:
    paths = (
        ROOT / ".dist/linux-static-release/sgrn_dataset",
        ROOT / ".dist/linux-static/sgrn_dataset",
        ROOT / ".prefix/bin/sgrn_dataset",
    )
    for p in paths:
        if p.is_file():
            return p
    raise FileNotFoundError("sgrn_dataset binary not found")


def ensure_dataset_csv(archive_path: Path, csv_path: Path) -> Path:
    if csv_path.is_file() and csv_path.stat().st_size > 0:
        return csv_path
    bin_path = find_sgrn_dataset_bin()
    cmd = [
        str(bin_path),
        "-s", str(SCL_SCHEMA),
        "--csv", str(csv_path),
        str(archive_path),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"sgrn_dataset failed: {res.stderr}")
    return csv_path


def load_dataset_df(csv_path: Path, features: Optional[list[str]] = None) -> Tuple[pd.DataFrame, pd.Series]:
    df = pd.read_csv(csv_path)
    # Drop startup empty frame (row 0)
    df = df.iloc[1:].reset_index(drop=True)

    # Simulated timestamps
    if "InletSeparation.timestamp" in df.columns:
        sim_timestamps = pd.to_datetime(df["InletSeparation.timestamp"])
    elif "timestamp_ms" in df.columns:
        sim_timestamps = pd.to_datetime(df["timestamp_ms"], unit="ms")
    elif "_time" in df.columns:
        sim_timestamps = pd.to_datetime(df["_time"])
    else:
        ts_col = next((c for c in df.columns if c.endswith(".timestamp")), df.columns[0])
        sim_timestamps = pd.to_datetime(df[ts_col])

    control_blacklist = {
        "timestamp", "esd_signal", "model_trip_advice", "model_anomaly",
        "model_fault_code", "esd_manual", "esd_auto", "esd_reason_code",
        "esd_time_ms", "active_alarm_count", "any_critical", "dew_point_forecast",
        "dew_point_forecast_std", "dew_point_spec", "trip_high_press", "trip_low_press",
        "trip_high_temp", "trip_high_dp",
    }

    if not features or features == "auto":
        features = [
            c for c in df.columns
            if not c.startswith("_")
            and c.split(".")[-1] not in control_blacklist
            and pd.api.types.is_numeric_dtype(df[c])
        ]
    else:
        # Check for renames: match exact or match by leaf if unambiguous
        resolved_features = []
        for f in features:
            if f in df.columns:
                resolved_features.append(f)
            else:
                leaf = f.split(".")[-1]
                matches = [c for c in df.columns if c.endswith("." + leaf) or c == leaf]
                if len(matches) == 1:
                    resolved_features.append(matches[0])
                else:
                    raise ValueError(f"Feature '{f}' not found in CSV dataset (available: {list(df.columns[:10])}...)")
        features = resolved_features

    feature_df = df[features].copy()
    feature_df = feature_df.ffill().bfill()
    return feature_df, sim_timestamps


def create_sliding_windows(
    arr: np.ndarray,
    window_size: int,
    stride: int = 1,
) -> np.ndarray:
    """Create sliding windows of shape (num_windows, window_size, num_features)."""
    n, d = arr.shape
    if n < window_size:
        return np.empty((0, window_size, d), dtype=np.float32)
    windows = []
    for i in range(0, n - window_size + 1, stride):
        windows.append(arr[i : i + window_size])
    return np.asarray(windows, dtype=np.float32)


def get_splits_and_windows(
    archive_path: Path | None = None,
    csv_path: Path | None = None,
    config: dict | None = None,
    meta: dict | None = None,
):
    if config is None:
        config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    if meta is None:
        meta = json.loads(META_PATH.read_text(encoding="utf-8"))

    flare_dir = Path(__file__).parent
    if archive_path is None:
        archive_path = flare_dir / "flare_history.jsonl.zst"
    if csv_path is None:
        csv_path = flare_dir / "dataset.csv"

    features_cfg = config.get("features")
    feature_df, sim_timestamps = load_dataset_df(csv_path, features_cfg)
    features = list(feature_df.columns)

    # Split chronologically around fault onset
    fault_onset_iso = meta["fault_onset_iso"]
    fault_onset_dt = pd.to_datetime(fault_onset_iso).tz_localize(None)
    sim_dt_naive = sim_timestamps.dt.tz_localize(None)

    pre_fault_mask = sim_dt_naive < fault_onset_dt
    fault_mask = sim_dt_naive >= fault_onset_dt

    pre_fault_values = feature_df[pre_fault_mask].to_numpy(dtype=np.float32)
    fault_values = feature_df[fault_mask].to_numpy(dtype=np.float32)
    pre_fault_ts = sim_timestamps[pre_fault_mask].reset_index(drop=True)
    fault_ts = sim_timestamps[fault_mask].reset_index(drop=True)

    n_pre = len(pre_fault_values)
    train_ratio = config.get("dataset", {}).get("train_ratio", 0.70)
    n_train = int(n_pre * train_ratio)

    train_raw = pre_fault_values[:n_train]
    cal_raw = pre_fault_values[n_train:]
    test_raw = fault_values

    # Fit scaler strictly on train_raw
    mean = np.mean(train_raw, axis=0)
    std = np.std(train_raw, axis=0)
    std = np.where(std < 1e-4, 1.0, std)  # guard against constant features
    scaler = Scaler(mean=mean, std=std, feature_names=features)

    train_norm = scaler.transform(train_raw)
    cal_norm = scaler.transform(cal_raw)
    test_norm = scaler.transform(test_raw)

    window_size = config.get("dataset", {}).get("window_size", 12)
    stride = config.get("dataset", {}).get("stride", 1)

    X_train = create_sliding_windows(train_norm, window_size, stride)
    X_cal = create_sliding_windows(cal_norm, window_size, stride)

    # For test: include calibration tail so test windows start from the very first fault observation
    test_context = np.vstack([cal_norm[-(window_size - 1) :], test_norm])
    X_test = create_sliding_windows(test_context, window_size, stride)
    test_timestamps = fault_ts.tolist()

    return {
        "scaler": scaler,
        "features": features,
        "X_train": X_train,
        "X_cal": X_cal,
        "X_test": X_test,
        "train_raw": train_raw,
        "cal_raw": cal_raw,
        "test_raw": test_raw,
        "test_timestamps": test_timestamps,
        "meta": meta,
        "config": config,
    }


if __name__ == "__main__":
    splits = get_splits_and_windows()
    print("Dataset processed successfully:")
    print(f"  Features: {len(splits['features'])}")
    print(f"  X_train windows: {splits['X_train'].shape}")
    print(f"  X_cal windows:   {splits['X_cal'].shape}")
    print(f"  X_test windows:  {splits['X_test'].shape}")
