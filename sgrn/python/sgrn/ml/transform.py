#!/usr/bin/env python3
"""
sgrn.ml.transform — Process Telemetry Preprocessing, Scaling & Sliding Window Generation.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple, Union

import numpy as np

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False
    pd = None


@dataclass
class Scaler:
    """Standardisation scaler (Z-score normalisation: x_norm = (x - mean) / std).
    
    Fitted strictly on pre-fault training splits to prevent data leakage.
    Ensures dimensionless unit variance across heterogeneous physical quantities.
    """

    mean: np.ndarray
    std: np.ndarray
    feature_names: list[str]

    def transform(self, x: np.ndarray) -> np.ndarray:
        """Project physical engineering units into standard normal space N(0, 1)."""
        return (x - self.mean) / self.std

    def inverseTransform(self, x: np.ndarray) -> np.ndarray:
        """Denormalise standard units back to physical engineering units."""
        return x * self.std + self.mean

    # Backward-compatible alias
    inverse_transform = inverseTransform

    def toDict(self) -> dict:
        """Serialize normalization parameters to a JSON-compatible dictionary."""
        return {
            "mean": self.mean.tolist(),
            "std": self.std.tolist(),
            "feature_names": self.feature_names,
        }

    # Backward-compatible alias
    to_dict = toDict

    @classmethod
    def fit(cls, data: np.ndarray, feature_names: Optional[list[str]] = None) -> Scaler:
        """Fit scaler parameters (mean, std) on training array."""
        arr = np.asarray(data, dtype=np.float32)
        mean = np.mean(arr, axis=0)
        std = np.std(arr, axis=0)
        std = np.where(std < 1e-4, 1.0, std)
        names = feature_names if feature_names is not None else [f"f{i}" for i in range(arr.shape[1])]
        return cls(mean=mean, std=std, feature_names=names)

    @classmethod
    def fromDict(cls, d: dict) -> Scaler:
        """Reconstruct scaler from parameter dictionary."""
        return cls(
            mean=np.array(d["mean"], dtype=np.float32),
            std=np.array(d["std"], dtype=np.float32),
            feature_names=d["feature_names"],
        )

    # Backward-compatible alias
    from_dict = fromDict


def filterProcessTimeline(
    data: Any,
    startTime: Optional[Any] = None,
    endTime: Optional[Any] = None,
    timestampCol: Optional[str] = None,
    timestamps: Optional[Any] = None,
) -> Tuple[Any, Optional[Any]]:
    """Filter telemetry by operational cycle boundaries or time window."""
    if startTime is None and endTime is None:
        return data, timestamps

    if HAS_PANDAS and isinstance(data, pd.DataFrame):
        ts_series = None
        if timestampCol and timestampCol in data.columns:
            ts_series = pd.to_datetime(data[timestampCol])
        elif timestamps is not None:
            ts_series = pd.to_datetime(timestamps)

        if ts_series is not None:
            mask = pd.Series(True, index=data.index)
            if startTime is not None:
                start_dt = pd.to_datetime(startTime)
                mask = mask & (ts_series >= start_dt)
            if endTime is not None:
                end_dt = pd.to_datetime(endTime)
                mask = mask & (ts_series <= end_dt)

            filtered_df = data[mask].copy().reset_index(drop=True)
            filtered_ts = ts_series[mask].reset_index(drop=True)
            return filtered_df, filtered_ts

    if isinstance(data, np.ndarray) and timestamps is not None:
        ts_arr = np.asarray(timestamps)
        mask = np.ones(len(data), dtype=bool)
        if startTime is not None:
            mask = mask & (ts_arr >= startTime)
        if endTime is not None:
            mask = mask & (ts_arr <= endTime)
        return data[mask], ts_arr[mask]

    return data, timestamps


def extractFeatureMatrix(
    data: Any,
    features: list[str],
) -> np.ndarray:
    """Extract and validate continuous numeric process feature matrix in float32."""
    if HAS_PANDAS and isinstance(data, pd.DataFrame):
        missing = [f for f in features if f not in data.columns]
        if missing:
            raise KeyError(f"Requested features missing from dataset: {missing}")
        feature_df = data[features].copy()
        feature_df = feature_df.ffill().bfill()
        return feature_df.to_numpy(dtype=np.float32)

    if isinstance(data, dict):
        missing = [f for f in features if f not in data]
        if missing:
            raise KeyError(f"Requested features missing from data dict: {missing}")
        cols = [np.asarray(data[f], dtype=np.float32) for f in features]
        return np.column_stack(cols)

    if isinstance(data, np.ndarray):
        if data.ndim != 2 or data.shape[1] != len(features):
            raise ValueError(
                f"Array shape {data.shape} does not match feature count {len(features)}"
            )
        return np.asarray(data, dtype=np.float32)

    raise TypeError(f"Unsupported data type for feature extraction: {type(data)}")


def splitProcessTimeline(
    data: np.ndarray,
    trainRatio: float = 0.70,
    calRatio: float = 0.30,
    faultOnsetIdx: Optional[int] = None,
    timestamps: Optional[Any] = None,
    faultOnsetTime: Optional[Any] = None,
) -> Dict[str, Any]:
    """Chronologically split telemetry into nominal training, conformal calibration, and test splits."""
    n_samples = len(data)
    ts_arr = np.asarray(timestamps) if timestamps is not None else None

    cutoff_idx = n_samples
    if faultOnsetIdx is not None:
        cutoff_idx = min(n_samples, int(faultOnsetIdx))
    elif faultOnsetTime is not None and ts_arr is not None:
        if HAS_PANDAS:
            ts_pdt = pd.to_datetime(ts_arr)
            cutoff_dt = pd.to_datetime(faultOnsetTime)
            mask = ts_pdt >= cutoff_dt
            if np.any(mask):
                cutoff_idx = int(np.argmax(mask))
        else:
            mask = ts_arr >= faultOnsetTime
            if np.any(mask):
                cutoff_idx = int(np.argmax(mask))

    pre_fault_data = data[:cutoff_idx]
    test_raw = data[cutoff_idx:]

    n_pre = len(pre_fault_data)
    n_train = int(n_pre * trainRatio)

    train_raw = pre_fault_data[:n_train]
    cal_raw = pre_fault_data[n_train:]

    train_ts = ts_arr[:n_train] if ts_arr is not None else None
    cal_ts = ts_arr[n_train:cutoff_idx] if ts_arr is not None else None
    test_ts = ts_arr[cutoff_idx:] if ts_arr is not None else None

    return {
        "trainRaw": train_raw,
        "calRaw": cal_raw,
        "testRaw": test_raw,
        "trainTs": train_ts,
        "calTs": cal_ts,
        "testTs": test_ts,
        # Backward-compatible snake_case keys
        "train_raw": train_raw,
        "cal_raw": cal_raw,
        "test_raw": test_raw,
        "train_ts": train_ts,
        "cal_ts": cal_ts,
        "test_ts": test_ts,
    }


def fitProcessScaler(
    trainData: np.ndarray,
    features: list[str],
    minStd: float = 1e-4,
) -> Scaler:
    """Fit Z-score standardisation scaler strictly on nominal training telemetry."""
    mean = np.mean(trainData, axis=0)
    std = np.std(trainData, axis=0)
    std = np.where(std < minStd, 1.0, std)
    return Scaler(mean=mean, std=std, feature_names=features)


def createSlidingWindows(
    arr: np.ndarray,
    windowSize: int = 12,
    stride: int = 1,
) -> np.ndarray:
    """Construct sliding temporal observation windows of shape (N_windows, window_size, num_features)."""
    n, d = arr.shape
    if n < windowSize:
        return np.empty((0, windowSize, d), dtype=np.float32)

    windows = []
    for i in range(0, n - windowSize + 1, stride):
        windows.append(arr[i : i + windowSize])
    return np.asarray(windows, dtype=np.float32)


# Backward-compatible snake_case aliases
filter_process_timeline = filterProcessTimeline
extract_feature_matrix = extractFeatureMatrix
split_process_timeline = splitProcessTimeline
fit_process_scaler = fitProcessScaler
create_sliding_windows = createSlidingWindows
