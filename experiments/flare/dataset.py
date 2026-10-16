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
import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Tuple

import numpy as np
import pandas as pd

from config_loader import loadConfig, getConfigPath, _getWindowSize

ROOT = Path(__file__).resolve().parents[2]
SCL_SCHEMA = ROOT / "sgrn/lib/gateway/simulations/gas_processing/schema.scl"
CONFIG_PATH = getConfigPath(Path(__file__).parent)
META_PATH = Path(__file__).with_name("meta.json")


from sgrn.ml import Scaler, createSlidingWindows



def findSgrnDatasetBin() -> Path:
    paths = (
        ROOT / ".dist/linux-static-release/sgrn_dataset",
        ROOT / ".dist/linux-static/sgrn_dataset",
        ROOT / ".prefix/bin/sgrn_dataset",
    )
    for p in paths:
        if p.is_file():
            return p
    raise FileNotFoundError("sgrn_dataset binary not found")


def ensureDatasetCsv(archive_path: Path, csv_path: Path) -> Path:
    if csv_path.is_file() and csv_path.stat().st_size > 0:
        return csv_path
    bin_path = findSgrnDatasetBin()
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


def resolveModelDir(model_dir: Path) -> Path:
    """Locate model.pt + model_meta.json under ``model_dir`` or its ``model/`` subfolder."""
    model_dir = Path(model_dir)
    if (model_dir / "model_meta.json").is_file():
        if model_dir.name != "model":
            print(f"[flare] NOTE: using flat artifact layout at {model_dir}")
        return model_dir
    sub = model_dir / "model"
    if (sub / "model_meta.json").is_file():
        print(f"[flare] NOTE: artifacts live in the 'model/' subfolder: {sub}")
        return sub
    raise FileNotFoundError(
        f"No model_meta.json under {model_dir} nor {sub}; "
        "train first (train_detector.py --save-dir <dir> writes <dir>/model/).")


def loadDatasetDf(csv_path: Path, features: Optional[list[str]] = None,
                    fillLimit: Optional[int] = 5) -> Tuple[pd.DataFrame, pd.Series]:
    df = pd.read_csv(csv_path)
    # Drop startup empty frame (row 0)
    df = df.iloc[1:].reset_index(drop=True)
    df = pd.read_csv(csv_path)
    # Drop startup empty frame (row 0)
    df = df.iloc[1:].reset_index(drop=True)

    # Simulated timestamps. Unset DTL fields render as year-0 strings
    # ("0000-00-00 ...") which no datetime parser accepts: coerce those to
    # NaT and drop the rows — a sample with no time has no temporal
    # position and cannot enter chronological modeling.
    if "InletSeparation.timestamp" in df.columns:
        sim_timestamps = pd.to_datetime(df["InletSeparation.timestamp"], errors="coerce")
    elif "timestamp_ms" in df.columns:
        sim_timestamps = pd.to_datetime(df["timestamp_ms"], unit="ms", errors="coerce")
    elif "_time" in df.columns:
        sim_timestamps = pd.to_datetime(df["_time"], errors="coerce")
    else:
        ts_col = next((c for c in df.columns if c.endswith(".timestamp")), df.columns[0])
        sim_timestamps = pd.to_datetime(df[ts_col], errors="coerce")
    n_bad_ts = int(sim_timestamps.isna().sum())
    if n_bad_ts:
        print(f"  Dropping {n_bad_ts} rows with unparseable timestamps (unset DTL renders as year 0)")
        keep = sim_timestamps.notna().to_numpy()
        df = df[keep].reset_index(drop=True)
        sim_timestamps = sim_timestamps[keep].reset_index(drop=True)
    if len(df) == 0:
        raise ValueError(f"No rows with usable timestamps in {csv_path}")

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
    # Time-aware fill: bounded forward-fill only. Backward fill would copy
    # future observations into the past (temporal leak in replay scoring),
    # so leading gaps are left as NaN here and resolved split-aware below
    # with train-split medians (no test data involved).
    feature_df = feature_df.ffill(limit=fillLimit)
    return feature_df, sim_timestamps


def getSplitsAndWindows(
    archive_path: Path | None = None,
    csv_path: Path | None = None,
    config: dict | None = None,
    meta: dict | None = None,
):
    if config is None:
        config = loadConfig(CONFIG_PATH)
    if meta is None:
        meta = json.loads(META_PATH.read_text(encoding="utf-8"))

    flare_dir = Path(__file__).parent
    if archive_path is None:
        archive_path = flare_dir / "flare_history.jsonl.zst"
    if csv_path is None:
        csv_path = flare_dir / "dataset.csv"

    features_cfg = config.get("features")
    fill_limit = config.get("dataset", {}).get("fill_limit", 5)
    feature_df, sim_timestamps = loadDatasetDf(csv_path, features_cfg, fillLimit=fill_limit)
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

    if n_pre == 0:
        raise ValueError("No pre-fault rows before fault onset; check fault_onset_iso vs dataset range")
    if n_train == 0:
        raise ValueError(f"Empty training split (n_pre={n_pre}, train_ratio={train_ratio})")

    # Boolean-mask slices may be read-only views; take owning copies for filling.
    train_raw = np.array(pre_fault_values[:n_train], dtype=np.float32, copy=True)
    cal_raw = np.array(pre_fault_values[n_train:], dtype=np.float32, copy=True)
    test_raw = np.array(fault_values, dtype=np.float32, copy=True)

    # Leading gaps (unfilled by the bounded ffill) resolve against the
    # TRAIN split median only — test/calibration data never leaks in.
    # Residual NaNs (outages longer than the fill limit) are left in place
    # and counted in the fill report instead of being paved over.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        train_median = np.nanmedian(train_raw, axis=0)
    dead = [features[j] for j in range(len(features)) if np.isnan(train_median[j])]
    if dead:
        raise ValueError(f"Features with no valid training samples (dead sensors?): {dead}")

    def _fill_leading(block: np.ndarray) -> int:
        """Fill leading NaNs per column with the train median; return filled count."""
        filled = 0
        for j in range(block.shape[1]):
            col = block[:, j]
            valid = np.nonzero(~np.isnan(col))[0]
            if len(valid) == 0:
                continue  # all-NaN block column: left visible (train dead already rejected)
            n_lead = int(valid[0])
            if n_lead:
                col[:n_lead] = train_median[j]
                filled += n_lead
        return filled

    train_leading = _fill_leading(train_raw)
    _fill_leading(cal_raw)
    _fill_leading(test_raw)
    fill_report = {
        "fill_limit": config.get("dataset", {}).get("fill_limit", 5),
        "train_leading_filled": train_leading,
        "residual_nans": int(np.isnan(train_raw).sum() + np.isnan(cal_raw).sum()
                             + np.isnan(test_raw).sum()),
    }

    # Fit scaler strictly on train_raw (NaN-aware: residual NaNs skipped)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        mean = np.nanmean(train_raw, axis=0)
        std = np.nanstd(train_raw, axis=0)
    std = np.where(std < 1e-4, 1.0, std)  # guard against constant features
    scaler = Scaler(mean=mean, std=std, feature_names=features)

    train_norm = scaler.transform(train_raw)
    cal_norm = scaler.transform(cal_raw)
    test_norm = scaler.transform(test_raw)

    window_size = _getWindowSize(config)
    stride = config.get("dataset", {}).get("stride", 1)

    X_train = createSlidingWindows(train_norm, window_size, stride)
    X_cal = createSlidingWindows(cal_norm, window_size, stride)

    # For test: include calibration tail so test windows start from the very first fault observation.
    # Note: cal_norm[-(window_size - 1):] with window_size=1 is the WHOLE array — slice explicitly.
    if window_size > 1 and len(cal_norm) > 0:
        test_context = np.vstack([cal_norm[max(0, len(cal_norm) - (window_size - 1)) :], test_norm])
    else:
        test_context = test_norm
    X_test = createSlidingWindows(test_context, window_size, stride)
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
        "fill_report": fill_report,
        "meta": meta,
        "config": config,
    }


if __name__ == "__main__":
    splits = getSplitsAndWindows()
    print("Dataset processed successfully:")
    print(f"  Features: {len(splits['features'])}")
    print(f"  Fill report: {splits['fill_report']}")
    print(f"  X_train windows: {splits['X_train'].shape}")
    print(f"  X_cal windows:   {splits['X_cal'].shape}")
    print(f"  X_test windows:  {splits['X_test'].shape}")
