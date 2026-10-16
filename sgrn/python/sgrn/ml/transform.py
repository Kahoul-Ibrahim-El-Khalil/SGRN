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
    fillLimit: Optional[int] = None,
    allowBackfill: bool = True,
) -> np.ndarray:
    """Extract and validate continuous numeric process feature matrix in float32.

    Missing-data policy (DataFrame input): forward-fill up to ``fillLimit``
    rows (``None`` = unbounded, legacy), then backward-fill leading gaps
    only when ``allowBackfill`` is true (legacy default). Backward fill
    copies FUTURE observations into the past: harmless for offline training
    matrices but a temporal leak in replay/online scoring — pass
    ``allowBackfill=False`` there and handle leading NaNs explicitly
    (e.g. train-split medians fitted without test data). Gaps longer than
    ``fillLimit`` are left as NaN so long outages stay visible instead of
    being silently paved over.
    """
    if HAS_PANDAS and isinstance(data, pd.DataFrame):
        missing = [f for f in features if f not in data.columns]
        if missing:
            raise KeyError(f"Requested features missing from dataset: {missing}")
        feature_df = data[features].copy()
        feature_df = feature_df.ffill(limit=fillLimit)
        if allowBackfill:
            feature_df = feature_df.bfill()
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


# ======================================================================================
# Discrete operating-context encoding (Bool flags + #ENUM integers).
# ======================================================================================
#
# Continuous manifold features are z-scored; discrete modes are NOT — a
# STOPPED=0 vs RUNNING=2 gap is not "twice as anomalous" as 1. Instead each
# context field is one-hot encoded per timestep (Bool -> single 0/1 column;
# enum with K categories -> K + 1 columns, the last meaning unknown/other),
# and the flattened window code conditions the autoencoder. The decoder
# reconstructs continuous signals only, so code distance never pollutes the
# anomaly score; unseen modes surface via the unknown column instead.

ContextSpec = Dict[str, Any]  # {"kind": "bool"|"enum", "categories": [...]}


def contextLayout(
    context_features: List[str],
    context_specs: Dict[str, ContextSpec],
) -> List[Dict[str, Any]]:
    """Column layout of the encoded context matrix: [(feat, kind, offset, width)]."""
    layout = []
    offset = 0
    for feat in context_features:
        spec = context_specs.get(feat, {"kind": "bool", "categories": ["false", "true"]})
        width = 1 if spec.get("kind") == "bool" else len(spec.get("categories", [])) + 1
        layout.append({"feature": feat, "kind": spec.get("kind", "bool"),
                       "offset": offset, "width": width})
        offset += width
    return layout


def contextEncodedDim(
    context_features: List[str],
    context_specs: Dict[str, ContextSpec],
) -> int:
    """Total one-hot width of the encoded context vector per timestep."""
    return sum(
        1 if context_specs.get(f, {}).get("kind") == "bool"
        else len(context_specs.get(f, {}).get("categories", [])) + 1
        for f in context_features
    )


def _encodeContextValue(value: Any, spec: ContextSpec) -> Tuple[List[float], bool]:
    """Encode one raw discrete value -> (one-hot code, unknown_flag)."""
    kind = spec.get("kind", "bool")
    if kind == "bool":
        if value is None or (isinstance(value, float) and np.isnan(value)):
            return [0.0], False
        if isinstance(value, str):
            v = value.strip().lower()
            if v in ("true", "1", "yes", "on"):
                return [1.0], False
            if v in ("false", "0", "no", "off", ""):
                return [0.0], False
            return [0.0], True
        return [1.0 if bool(value) else 0.0], False
    cats = [str(c) for c in spec.get("categories", [])]
    width = len(cats) + 1
    code = [0.0] * width
    if value is None or (isinstance(value, float) and np.isnan(value)):
        code[-1] = 1.0
        return code, True
    if isinstance(value, str):
        v = value.strip()
        if v in cats:
            code[cats.index(v)] = 1.0
            return code, False
        if v.lstrip("-").isdigit() and 0 <= int(v) < len(cats):
            code[int(v)] = 1.0
            return code, False
        code[-1] = 1.0
        return code, True
    if isinstance(value, bool):
        idx = int(value)
        if idx < len(cats):
            code[idx] = 1.0
            return code, False
        code[-1] = 1.0
        return code, True
    try:
        idx = int(value)
    except (TypeError, ValueError):
        code[-1] = 1.0
        return code, True
    if 0 <= idx < len(cats):
        code[idx] = 1.0
        return code, False
    code[-1] = 1.0
    return code, True


def extractRawContextColumns(data: Any, context_features: List[str]) -> np.ndarray:
    """Extract raw (unencoded) discrete context columns as an object matrix (n, n_ctx)."""
    if not context_features:
        n = len(data) if hasattr(data, "__len__") else 0
        return np.empty((n, 0), dtype=object)
    if HAS_PANDAS and isinstance(data, pd.DataFrame):
        missing = [f for f in context_features if f not in data.columns]
        if missing:
            raise KeyError(f"Requested context signals missing from dataset: {missing}")
        cols = []
        for f in context_features:
            col = data[f]
            if isinstance(col.dtype, pd.CategoricalDtype):
                col = col.astype(object)
            cols.append(col.to_numpy(dtype=object, na_value=None))
        return np.column_stack(cols)
    if isinstance(data, dict):
        missing = [f for f in context_features if f not in data]
        if missing:
            raise KeyError(f"Requested context signals missing from data dict: {missing}")
        return np.column_stack(
            [np.asarray(data[f], dtype=object) for f in context_features]
        )
    if isinstance(data, np.ndarray):
        arr = np.asarray(data, dtype=object)
        if arr.ndim != 2 or arr.shape[1] != len(context_features):
            raise ValueError(
                f"Array shape {arr.shape} does not match context count {len(context_features)}"
            )
        return arr
    raise TypeError(f"Unsupported data type for context extraction: {type(data)}")


def inferContextSpecs(
    raw_columns: np.ndarray,
    context_features: List[str],
    maxCardinality: int = 32,
) -> Dict[str, ContextSpec]:
    """Infer Bool/enum specs from nominal training columns (no-schema path).

    All-bool columns -> ``bool``; integer columns with 2..maxCardinality
    distinct values -> ``enum`` with sorted-observed categories; anything
    else raises (continuous data does not belong in the context path).
    """
    specs: Dict[str, ContextSpec] = {}
    for j, feat in enumerate(context_features):
        col = raw_columns[:, j]
        vals = [v for v in col if v is not None and not (isinstance(v, float) and np.isnan(v))]
        if all(isinstance(v, (bool, np.bool_)) for v in vals):
            specs[feat] = {"kind": "bool", "categories": ["false", "true"]}
            continue
        str_vals = {str(v).strip() for v in vals}
        non_numeric = [v for v in str_vals if not v.lstrip("-").isdigit()]
        if non_numeric and len(str_vals) <= maxCardinality:
            specs[feat] = {"kind": "enum", "categories": sorted(str_vals)}
            continue
        try:
            ints = sorted({int(float(v)) for v in vals})
        except (TypeError, ValueError):
            raise ValueError(
                f"Context signal '{feat}' is neither boolean nor low-cardinality "
                f"discrete ({len(str_vals)} distinct values); route continuous "
                f"data through features, not context."
            )
        if 2 <= len(ints) <= maxCardinality:
            specs[feat] = {"kind": "enum", "categories": [str(i) for i in ints]}
        else:
            raise ValueError(
                f"Context signal '{feat}' has {len(ints)} distinct integers "
                f"(allowed 2..{maxCardinality}); high-cardinality codes carry "
                f"no mode meaning — exclude it or raise maxCardinality."
            )
    return specs


def encodeContextMatrix(
    raw_columns: np.ndarray,
    context_features: List[str],
    context_specs: Dict[str, ContextSpec],
) -> Tuple[np.ndarray, np.ndarray]:
    """One-hot encode raw context columns -> (encoded float32 (n, C), unknown bool (n,)).

    A row is unknown when any enum field hits its other/unknown column —
    i.e. a category never seen in the spec (schema #ENUM or nominal train).
    """
    n = raw_columns.shape[0] if raw_columns.ndim == 2 else 0
    dim = contextEncodedDim(context_features, context_specs)
    encoded = np.zeros((n, dim), dtype=np.float32)
    unknown = np.zeros(n, dtype=bool)
    layout = contextLayout(context_features, context_specs)
    for j, entry in enumerate(layout):
        spec = context_specs.get(entry["feature"], {"kind": "bool"})
        off, width = entry["offset"], entry["width"]
        for i in range(n):
            code, is_unknown = _encodeContextValue(raw_columns[i, j], spec)
            encoded[i, off : off + width] = code
            unknown[i] = unknown[i] or is_unknown
    return encoded, unknown


def extractContextMatrix(
    data: Any,
    context_features: List[str],
    context_specs: Dict[str, ContextSpec],
) -> Tuple[np.ndarray, np.ndarray]:
    """Extract + one-hot encode discrete context from a DataFrame/dict/array."""
    raw = extractRawContextColumns(data, context_features)
    return encodeContextMatrix(raw, context_features, context_specs)


# Backward-compatible snake_case aliases
filter_process_timeline = filterProcessTimeline
extract_feature_matrix = extractFeatureMatrix
split_process_timeline = splitProcessTimeline
fit_process_scaler = fitProcessScaler
create_sliding_windows = createSlidingWindows
context_layout = contextLayout
context_encoded_dim = contextEncodedDim
extract_raw_context_columns = extractRawContextColumns
infer_context_specs = inferContextSpecs
encode_context_matrix = encodeContextMatrix
extract_context_matrix = extractContextMatrix


# ======================================================================================
# Mode-stratified standardisation.
# ======================================================================================
#
# A conditional autoencoder can learn to ignore its context input: the
# continuous window already contains the answer, so reconstruction succeeds
# without ever reading the mode. Stratification closes that bypass — each
# timestep is z-scored by its own mode's nominal mean/std, so "temp 280
# while heater OFF" becomes a ~200-sigma residual in ANY downstream model,
# conditioned or not. Rows whose stratum is unknown or under-sampled fall
# back to global statistics (and are flagged, never silently absorbed).

def contextRowKeys(
    raw_columns: np.ndarray,
    context_features: List[str],
    context_specs: Dict[str, ContextSpec],
    fields: Optional[List[str]] = None,
) -> List[tuple]:
    """Per-row mode keys: category-name tuple over ``fields`` (default: all context fields)."""
    fields = list(fields) if fields is not None else list(context_features)
    idx = [context_features.index(f) for f in fields]
    keys = []
    for i in range(raw_columns.shape[0]):
        key = []
        for j, f in zip(idx, fields):
            spec = context_specs.get(f, {"kind": "bool"})
            code, _ = _encodeContextValue(raw_columns[i, j], spec)
            if spec.get("kind") == "bool":
                key.append("true" if code[0] >= 0.5 else "false")
            else:
                cats = [str(c) for c in spec.get("categories", [])]
                pos = int(np.argmax(code)) if code else -1
                key.append(cats[pos] if 0 <= pos < len(cats) else "unknown")
        keys.append(tuple(key))
    return keys


def fitStratifiedStats(
    cont_matrix: np.ndarray,
    row_keys: List[tuple],
    min_samples: int = 30,
    min_std: float = 1e-4,
) -> Tuple[Dict[str, Dict[str, List[float]]], Dict[str, List[float]]]:
    """Fit per-stratum (mean, std) on nominal rows; strata below ``min_samples`` fold to global.

    Returns ``(strata, global_stats)`` with JSON-serializable lists; keys are
    ``json.dumps``-encoded tuples.
    """
    import json as _json

    arr = np.asarray(cont_matrix, dtype=np.float32)
    global_mean = np.mean(arr, axis=0)
    global_std = np.std(arr, axis=0)
    global_std = np.where(global_std < min_std, 1.0, global_std)
    global_stats = {"mean": global_mean.tolist(), "std": global_std.tolist()}

    by_key: Dict[str, List[int]] = {}
    for i, k in enumerate(row_keys):
        by_key.setdefault(_json.dumps(list(k)), []).append(i)
    strata: Dict[str, Dict[str, List[float]]] = {}
    for k, rows in by_key.items():
        if len(rows) < min_samples:
            continue
        sub = arr[rows]
        m = np.mean(sub, axis=0)
        s = np.std(sub, axis=0)
        s = np.where(s < min_std, 1.0, s)
        strata[k] = {"mean": m.tolist(), "std": s.tolist(), "n": len(rows)}
    return strata, global_stats


def applyStratifiedStats(
    cont_matrix: np.ndarray,
    row_keys: List[tuple],
    strata: Dict[str, Dict[str, List[float]]],
    global_stats: Dict[str, List[float]],
) -> Tuple[np.ndarray, np.ndarray]:
    """Z-score each row by its stratum (global fallback) -> (scaled float32, fallback bool (n,))."""
    import json as _json

    arr = np.asarray(cont_matrix, dtype=np.float32)
    gmean = np.asarray(global_stats["mean"], dtype=np.float32)
    gstd = np.asarray(global_stats["std"], dtype=np.float32)
    out = np.empty_like(arr)
    fallback = np.zeros(arr.shape[0], dtype=bool)
    cache: Dict[str, Optional[tuple]] = {}
    for i, k in enumerate(row_keys):
        jk = _json.dumps(list(k))
        if jk not in cache:
            s = strata.get(jk)
            cache[jk] = (
                (np.asarray(s["mean"], dtype=np.float32),
                 np.asarray(s["std"], dtype=np.float32))
                if s is not None else None
            )
        entry = cache[jk]
        if entry is None:
            out[i] = (arr[i] - gmean) / gstd
            fallback[i] = True
        else:
            m, s = entry
            out[i] = (arr[i] - m) / s
    return out.astype(np.float32), fallback


context_row_keys = contextRowKeys
fit_stratified_stats = fitStratifiedStats
apply_stratified_stats = applyStratifiedStats


# ======================================================================================
# Streaming (online) statistics — Welford mean/variance in O(dim) memory.
# ======================================================================================
#
# Batch training materialises the full (N, F) matrix, then (N, W, F) windows
# (stride-1 replicates every row W times), then a torch copy of both. For
# archive-scale histories that is the OOM cliff. StreamingStats accumulates
# the same sufficient statistics in one ordered pass so the scaler, strata
# and mode/category sets are exact without holding any telemetry.

class StreamingStats:
    """Numerically stable online mean/variance (Welford) with exact merge.

    Missing (NaN) entries are skipped per-coordinate, so sparse gaps neither
    bias the mean nor shrink the variance.
    """

    def __init__(self, dim: int, min_std: float = 1e-4):
        self.dim = int(dim)
        self.min_std = float(min_std)
        self.counts = np.zeros(dim, dtype=np.int64)
        self.mean = np.zeros(dim, dtype=np.float64)
        self._m2 = np.zeros(dim, dtype=np.float64)

    @property
    def n(self) -> int:
        return int(self.counts.max()) if self.dim else 0

    def update(self, x: Any) -> None:
        v = np.asarray(x, dtype=np.float64).reshape(-1)
        if v.shape[0] != self.dim:
            raise ValueError(f"Expected dim {self.dim}, got {v.shape[0]}")
        for j in np.nonzero(np.isfinite(v))[0]:
            self.counts[j] += 1
            delta = v[j] - self.mean[j]
            self.mean[j] += delta / self.counts[j]
            self._m2[j] += delta * (v[j] - self.mean[j])

    @property
    def variance(self) -> np.ndarray:
        # Population variance (ddof=0) to match np.std used by fitProcessScaler.
        denom = np.maximum(self.counts, 1)
        return self._m2 / denom

    @property
    def std(self) -> np.ndarray:
        s = np.sqrt(np.maximum(self.variance, 0.0))
        return np.where(s < self.min_std, 1.0, s)

    def merge(self, other: "StreamingStats") -> "StreamingStats":
        """Combine two accumulators exactly (parallel Welford, per-coordinate)."""
        if other.dim != self.dim:
            raise ValueError("dim mismatch in merge")
        n2 = other.counts
        n1 = self.counts
        n = n1 + n2
        both = (n1 > 0) & (n2 > 0)
        delta = np.where(both, other.mean - self.mean, 0.0)
        self._m2 = (self._m2 + other._m2
                    + np.where(both, delta * delta * (n1 * n2 / np.maximum(n, 1)), 0.0))
        self.mean = np.where(both, self.mean + delta * (n2 / np.maximum(n, 1)),
                             np.where(n2 > 0, other.mean, self.mean))
        self.counts = n
        return self

    def toScaler(self, feature_names: Optional[List[str]] = None) -> "Scaler":
        names = feature_names if feature_names is not None else [f"f{i}" for i in range(self.dim)]
        return Scaler(mean=self.mean.astype(np.float32), std=self.std.astype(np.float32),
                      feature_names=names)


streaming_stats = StreamingStats
