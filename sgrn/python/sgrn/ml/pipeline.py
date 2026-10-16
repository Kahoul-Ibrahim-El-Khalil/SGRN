#!/usr/bin/env python3
"""
sgrn.ml.pipeline — High-Level End-to-End Manifold Training & Evaluation Pipelines.

Composes:
  - SCL Schema auto-configuration (sgrn.ml.schema)
  - Preprocessing, timeline filtering, and windowing (sgrn.ml.transform)
  - Hardware discovery (AMD ROCm / NVIDIA CUDA / MPS) (sgrn.ml.device)
  - PyTorch Autoencoder training and conformal calibration (sgrn.ml.manifold)
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional, Union

import numpy as np

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False
    pd = None

from .device import getHardwareInfo, resolveDevice
from .manifold import (
    ModePredictor,
    ProcessManifoldDetector,
    _conformalQuantile,
    _modeTargets,
    trainManifoldDetector,
)
from .schema import configureModelFromSchema
from .transform import (
    Scaler,
    StreamingStats,
    contextLayout,
    contextRowKeys,
    createSlidingWindows,
    encodeContextMatrix,
    extractContextMatrix,
    extractFeatureMatrix,
    extractRawContextColumns,
    filterProcessTimeline,
    fitProcessScaler,
    inferContextSpecs,
    splitProcessTimeline,
)


def fitManifoldPipeline(
    data: Any,
    features: Optional[List[str]] = None,
    schemaSource: Optional[Union[str, Path, Dict[str, Any]]] = None,
    windowSize: int = 12,
    stride: int = 1,
    cycleStart: Optional[Any] = None,
    cycleEnd: Optional[Any] = None,
    faultOnset: Optional[Any] = None,
    trainRatio: float = 0.70,
    calRatio: float = 0.30,
    timestampCol: Optional[str] = None,
    modelConfig: Optional[dict] = None,
    device: Optional[Union[str, Any]] = None,
    seed: int = 42,
    calibrationStride: Optional[int] = None,
    calibration_stride: Optional[int] = None,
    includeSubsystems: Optional[List[str]] = None,
    excludeSubsystems: Optional[List[str]] = None,
    includeDimensions: Optional[List[str]] = None,
    excludeSignals: Optional[List[str]] = None,
    useContext: bool = True,
    contextFeatures: Optional[List[str]] = None,
    contextData: Any = None,
    maxContextCardinality: int = 32,
    stratifyOn: Optional[Any] = None,
    minStratumSamples: int = 30,
    trainModeHead: bool = True,
    modeHeadConfig: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """Composable end-to-end functional pipeline that fits a Manifold Anomaly Detector.

    If `schemaSource` is provided and `features` is None, features and their physical
    dimensions are automatically discovered and configured directly from the SCL schema!

    Discrete operating context (``Bool`` flags, ``#ENUM`` modes) conditions the
    autoencoder when ``useContext`` is true (default): context signals come
    from the schema, from explicit ``contextFeatures``, or both. The decoder
    reconstructs continuous signals only. Pass ``useContext=False`` for the
    legacy continuous-only model. ``contextData`` supplies a separate
    row-aligned context source (required when ``data`` is a bare ndarray);
    otherwise context columns are read from ``data`` itself.

    ``stratifyOn`` selects the context fields whose modes define
    standardisation strata (default: all context fields; ``False`` disables).
    Each timestep is z-scored by its own mode's nominal mean/std, so a value
    that is nominal in one mode but impossible in another scores as a large
    residual in any model. Strata below ``minStratumSamples`` nominal rows
    fall back to global statistics, so over-fine plant-state combos degrade
    gracefully to the legacy global scaling instead of faking precision.

    Calibration uses non-overlapping windows (``calibrationStride``, default
    = ``windowSize``) so the conformal threshold is computed on
    approximately exchangeable block scores rather than autocorrelated
    stride-1 overlaps. Pass ``calibrationStride=1`` only to reproduce the
    legacy overlapping behaviour.

    Returns:
      - 'detector': Trained ProcessManifoldDetector
      - 'scaler': Fitted Scaler
      - 'splits': Dictionary of data splits
      - 'device': Active compute device string
      - 'features': List of signal names
      - 'featureDimensions': Physical dimensions per signal
      - 'featureSubsystems': Plant subsystem per signal
      - 'contextFeatures': Discrete conditioning signals (possibly [])
      - 'contextSpecs': Encoding specs per context signal
      - 'calibrationStride': Effective calibration thinning stride
      - 'nCalEffective': Number of windows actually used for calibration
    """
    if modelConfig is None:
        modelConfig = {}

    feature_dimensions = None
    feature_subsystems = None
    schema_context_specs: Dict[str, Any] = {}
    schema_timestamp_fields: List[str] = []

    # Step 0: Auto-configure features and dimensions from SCL schema if provided
    if schemaSource is not None:
        schema_cfg = configureModelFromSchema(
            schemaSource=schemaSource,
            windowSize=windowSize,
            includeSubsystems=includeSubsystems,
            excludeSubsystems=excludeSubsystems,
            includeDimensions=includeDimensions,
            excludeSignals=excludeSignals,
        )
        if features is None:
            features = schema_cfg["features"]
        feature_dimensions = schema_cfg["featureDimensions"]
        feature_subsystems = schema_cfg["featureSubsystems"]
        schema_context_specs = dict(schema_cfg.get("contextSpecs", {}))
        schema_timestamp_fields = list(schema_cfg.get("timestampFields", []))
        if contextFeatures is None:
            contextFeatures = list(schema_cfg.get("contextFeatures", []))
        if "model" not in modelConfig:
            modelConfig["model"] = schema_cfg["modelConfig"]

    if features is None:
        raise ValueError("Either 'features' list or 'schemaSource' path must be provided.")

    if not useContext:
        contextFeatures = []
    elif contextFeatures is None:
        contextFeatures = []

    # Timestamp auto-discovery: DTL/LDTL fields from the schema normalise
    # time directly (ns) with no CSV round-trip. Applies only when the data
    # source actually carries the column.
    effective_timestamp_col = timestampCol
    if effective_timestamp_col is None and schema_timestamp_fields:
        if HAS_PANDAS and isinstance(data, pd.DataFrame):
            hit = next((f for f in schema_timestamp_fields if f in data.columns), None)
            if hit is not None:
                effective_timestamp_col = hit
        elif isinstance(data, dict):
            hit = next((f for f in schema_timestamp_fields if f in data), None)
            if hit is not None:
                effective_timestamp_col = hit

    # Step 1: Filter dataset by process cycle boundaries
    filtered_data, timestamps = filterProcessTimeline(
        data=data,
        startTime=cycleStart,
        endTime=cycleEnd,
        timestampCol=effective_timestamp_col,
    )
    # filterProcessTimeline returns timestamps=None unless a separate
    # timestamps array is passed — recover the timestamp column here so a
    # timestamp faultOnset is honoured even without cycleStart/cycleEnd.
    if timestamps is None and effective_timestamp_col is not None and HAS_PANDAS and isinstance(
        filtered_data, pd.DataFrame
    ) and effective_timestamp_col in filtered_data.columns:
        timestamps = pd.to_datetime(filtered_data[effective_timestamp_col])

    # Step 2: Extract clean numeric feature matrix
    feat_matrix = extractFeatureMatrix(filtered_data, features)

    # Step 3: Chronological partition into nominal (train, cal) and test
    # Accept plain ints, numpy integers (not bools), and timestamp-like
    # fault onsets. The previous isinstance(..., int) check silently dropped
    # numpy integers; the timestamp branch silently dropped faultOnset when
    # timestamps was None (no cycle filter), leaking faults into train/cal.
    import numbers as _numbers

    _fault_idx = None
    _fault_time = None
    if isinstance(faultOnset, bool):
        pass
    elif isinstance(faultOnset, _numbers.Integral):
        _fault_idx = int(faultOnset)
    elif isinstance(faultOnset, (str, pd.Timestamp) if HAS_PANDAS else str):
        _fault_time = faultOnset
    elif HAS_PANDAS and isinstance(faultOnset, pd.Timestamp):
        _fault_time = faultOnset
    else:
        try:
            import datetime as _dt

            if isinstance(faultOnset, (_dt.datetime, _dt.date)):
                _fault_time = faultOnset
        except Exception:
            pass
        if _fault_time is None and faultOnset is not None:
            try:
                import numpy as _np

                if isinstance(faultOnset, _np.integer):
                    _fault_idx = int(faultOnset)
            except Exception:
                pass
    splits = splitProcessTimeline(
        data=feat_matrix,
        trainRatio=trainRatio,
        calRatio=calRatio,
        timestamps=timestamps,
        faultOnsetTime=_fault_time,
        faultOnsetIdx=_fault_idx,
    )

    # Step 3b: Discrete operating context — same chronological cutoff as the
    # continuous split (identical args + row counts -> identical cutoff, so no
    # fault-label leakage between context and telemetry).
    context_specs: Dict[str, Any] = {}
    ctx_train_raw = ctx_cal_raw = ctx_test_raw = None
    if contextFeatures:
        ctx_source = data if contextData is None else contextData
        if ctx_source is not data:
            if (cycleStart is not None or cycleEnd is not None) and HAS_PANDAS and isinstance(
                ctx_source, pd.DataFrame
            ) and (effective_timestamp_col is None or effective_timestamp_col not in ctx_source.columns):
                raise ValueError(
                    "contextData must carry the timestamp column "
                    f"'{effective_timestamp_col}' when cycleStart/cycleEnd filter rows; "
                    "otherwise context rows misalign with telemetry."
                )
            filtered_ctx, _ = filterProcessTimeline(
                data=ctx_source,
                startTime=cycleStart,
                endTime=cycleEnd,
                timestampCol=effective_timestamp_col,
            )
            if len(filtered_ctx) != len(filtered_data):
                raise ValueError(
                    f"contextData rows ({len(filtered_ctx)}) != data rows "
                    f"({len(filtered_data)}) after filtering; sources must align."
                )
        else:
            filtered_ctx = filtered_data
        raw_ctx_all = extractRawContextColumns(filtered_ctx, contextFeatures)
        ctx_splits = splitProcessTimeline(
            data=np.asarray(raw_ctx_all, dtype=object),
            trainRatio=trainRatio,
            calRatio=calRatio,
            timestamps=timestamps,
            faultOnsetTime=_fault_time,
            faultOnsetIdx=_fault_idx,
        )
        ctx_train_raw, ctx_cal_raw, ctx_test_raw = (
            ctx_splits["trainRaw"],
            ctx_splits["calRaw"],
            ctx_splits["testRaw"],
        )
        # Specs: schema-known first; anything explicitly requested but not in
        # the schema (e.g. overridden feedback registers) is inferred from
        # nominal training columns — never from cal/test.
        missing = [f for f in contextFeatures if f not in schema_context_specs]
        if missing:
            idx = [contextFeatures.index(f) for f in missing]
            inferred = inferContextSpecs(
                np.asarray(ctx_train_raw)[:, idx],
                missing,
                maxCardinality=maxContextCardinality,
            )
        else:
            inferred = {}
        context_specs = {**{f: schema_context_specs[f] for f in contextFeatures
                            if f in schema_context_specs}, **inferred}

    # Step 4: Fit scaler strictly on nominal training telemetry
    scaler = fitProcessScaler(splits["trainRaw"], features)

    # Step 5: Standardise splits and create sliding windows. With active
    # strata each row is z-scored by its own mode's nominal stats (closing
    # the conditional-autoencoder bypass); otherwise global scaling.
    if stratifyOn is False:
        stratify_fields: List[str] = []
    elif isinstance(stratifyOn, list):
        stratify_fields = [f for f in stratifyOn if f in (contextFeatures or [])]
    else:
        stratify_fields = list(contextFeatures or [])
    strata_stats: Dict[str, Any] = {}
    if stratify_fields and ctx_train_raw is not None:
        from .transform import contextRowKeys as _keys, fitStratifiedStats as _fit
        _train_keys = _keys(np.asarray(ctx_train_raw, dtype=object),
                            contextFeatures, context_specs, fields=stratify_fields)
        strata_stats, _ = _fit(np.asarray(splits["trainRaw"], dtype=np.float32),
                               _train_keys, min_samples=minStratumSamples)
        if not strata_stats:
            print("[stratify] No stratum reached "
                  f"{minStratumSamples} nominal rows; using global scaling")
            stratify_fields = []
        else:
            print(f"[stratify] {len(strata_stats)} mode strata over {stratify_fields}")

    def _standardise(raw_mat: np.ndarray, ctx_raw: Any) -> np.ndarray:
        if strata_stats and stratify_fields and ctx_raw is not None:
            from .transform import applyStratifiedStats as _apply2, contextRowKeys as _keys2
            keys = _keys2(np.asarray(ctx_raw, dtype=object),
                          contextFeatures, context_specs, fields=stratify_fields)
            scaled, _ = _apply2(np.asarray(raw_mat, dtype=np.float32), keys, strata_stats,
                                {"mean": scaler.mean.tolist(), "std": scaler.std.tolist()})
            return scaled
        return scaler.transform(raw_mat)

    train_mat = _standardise(splits["trainRaw"], ctx_train_raw)
    cal_mat = _standardise(splits["calRaw"], ctx_cal_raw)

    X_train = createSlidingWindows(train_mat, windowSize=windowSize, stride=stride)
    X_cal = createSlidingWindows(cal_mat, windowSize=windowSize, stride=stride)

    splits["X_train"] = X_train
    splits["X_cal"] = X_cal

    X_ctx_train = X_ctx_cal = X_ctx_test = None
    if contextFeatures:
        C_train, _ = encodeContextMatrix(
            np.asarray(ctx_train_raw, dtype=object), contextFeatures, context_specs
        )
        C_cal, _ = encodeContextMatrix(
            np.asarray(ctx_cal_raw, dtype=object), contextFeatures, context_specs
        )
        X_ctx_train = createSlidingWindows(C_train, windowSize=windowSize, stride=stride)
        X_ctx_cal = createSlidingWindows(C_cal, windowSize=windowSize, stride=stride)
        splits["contextTrainRaw"] = ctx_train_raw
        splits["contextCalRaw"] = ctx_cal_raw
        splits["contextTestRaw"] = ctx_test_raw
        splits["X_ctx_train"] = X_ctx_train
        splits["X_ctx_cal"] = X_ctx_cal

    if len(splits["testRaw"]) > 0:
        test_mat = _standardise(splits["testRaw"], ctx_test_raw)
        # Last (windowSize-1) calibration rows as left context. Note
        # cal_mat[-(windowSize-1):] with windowSize=1 is cal_mat[-0:],
        # i.e. the WHOLE array — slice explicitly instead.
        if windowSize > 1 and len(cal_mat) > 0:
            _ctx = cal_mat[max(0, len(cal_mat) - (windowSize - 1)) :]
            test_context = np.vstack([_ctx, test_mat])
        else:
            test_context = test_mat
        splits["X_test"] = createSlidingWindows(test_context, windowSize=windowSize, stride=stride)
        # NOTE: X_test above is NORMALISED (z-scored) windows for direct use
        # with detector.computeWindowError. detector.detectBatch /
        # evaluateManifoldSequence expect RAW engineering-unit windows and
        # normalise internally — feeding X_test into detectBatch would
        # double-normalise. Use splits["testRaw"] with
        # evaluateManifoldSequence for the raw path.
        if contextFeatures:
            C_test, _ = encodeContextMatrix(
                np.asarray(ctx_test_raw, dtype=object), contextFeatures, context_specs
            )
            if windowSize > 1 and len(C_cal) > 0:
                _cctx = C_cal[max(0, len(C_cal) - (windowSize - 1)) :]
                C_test_context = np.vstack([_cctx, C_test])
            else:
                C_test_context = C_test
            # NOTE: encoded context is NOT z-scored, so X_ctx_test aligns
            # row-for-row with the normalised X_test above by construction.
            X_ctx_test = createSlidingWindows(C_test_context, windowSize=windowSize, stride=stride)
            assert len(X_ctx_test) == len(splits["X_test"]), (
                f"context test windows ({len(X_ctx_test)}) != continuous "
                f"({len(splits['X_test'])}); windowing diverged"
            )
            splits["X_ctx_test"] = X_ctx_test
    else:
        splits["X_test"] = np.empty((0, windowSize, len(features)), dtype=np.float32)
        if contextFeatures:
            splits["X_ctx_test"] = np.empty((0, windowSize, 0), dtype=np.float32)

    # Step 6: Train autoencoder and calibrate split-conformal bounds (accelerated on GPU/ROCm)
    # Default calibration stride = one window length (disjoint blocks for
    # approximate exchangeability); explicit 1 restores legacy overlaps.
    if calibrationStride is None:
        calibrationStride = calibration_stride
    eff_cal_stride = calibrationStride if calibrationStride is not None else windowSize
    detector = trainManifoldDetector(
        X_train=X_train,
        X_cal=X_cal,
        scaler=scaler,
        features=features,
        config=modelConfig,
        seed=seed,
        device=device,
        featureDimensions=feature_dimensions,
        featureSubsystems=feature_subsystems,
        calibrationStride=eff_cal_stride,
        X_ctx_train=X_ctx_train,
        X_ctx_cal=X_ctx_cal,
        context_features=contextFeatures if contextFeatures else None,
        context_specs=context_specs or None,
        stratify_fields=stratify_fields or None,
        strata_stats=strata_stats or None,
        trainModeHead=trainModeHead,
        modeHeadConfig=modeHeadConfig,
    )
    n_cal_effective = len(X_cal[:: max(1, int(eff_cal_stride))]) if len(X_cal) else 0
    train_config = {
        "epochs": modelConfig.get("model", {}).get("epochs", 50),
        "seed": seed,
        "alpha": modelConfig.get("model", {}).get("alpha", 0.05),
        "window_size": windowSize,
        "stride": stride,
        "calibration_stride": eff_cal_stride,
        "n_train_windows": len(X_train),
        "n_cal_effective": n_cal_effective,
        "n_features": len(features),
        "n_context": len(contextFeatures or []),
    }

    return {
        "detector": detector,
        "scaler": scaler,
        "splits": splits,
        "device": str(detector.device),
        "features": features,
        "windowSize": windowSize,
        "featureDimensions": feature_dimensions,
        "featureSubsystems": feature_subsystems,
        "contextFeatures": list(contextFeatures),
        "contextSpecs": dict(context_specs),
        "timestampFields": list(schema_timestamp_fields),
        "stratifyFields": list(stratify_fields),
        "strata": {k: v.get("n", 0) for k, v in strata_stats.items()},
        "trainConfig": train_config,
        "calibrationStride": eff_cal_stride,
        "nCalEffective": n_cal_effective,
        # snake_case backward-compatibility keys
        "window_size": windowSize,
        "feature_dimensions": feature_dimensions,
        "feature_subsystems": feature_subsystems,
        "context_features": list(contextFeatures),
        "context_specs": dict(context_specs),
        "stratify_fields": list(stratify_fields),
        "train_config": train_config,
        "calibration_stride": eff_cal_stride,
        "n_cal_effective": n_cal_effective,
    }


def evaluateManifoldSequence(
    detector: ProcessManifoldDetector,
    data: Any,
    features: Optional[List[str]] = None,
    windowSize: Optional[int] = None,
    batchSize: int = 256,
    device: Optional[Union[str, Any]] = None,
    contextData: Any = None,
    contextFeatures: Optional[List[str]] = None,
) -> Dict[str, Any]:
    """Evaluate an entire continuous dataset sequence with GPU-accelerated batch scoring.

    Discrete context is read from ``contextData`` when given, else from
    ``data`` itself, using the detector's configured context features/specs
    unless ``contextFeatures`` overrides them.
    """
    if features is None:
        features = detector.features
    if windowSize is None:
        windowSize = detector.window_size

    if device is not None:
        detector.to(device)

    feat_matrix = extractFeatureMatrix(data, features)
    windows = createSlidingWindows(feat_matrix, windowSize=windowSize, stride=1)

    ctx_windows = None
    ctx_feats = contextFeatures if contextFeatures is not None else detector.context_features
    if ctx_feats:
        ctx_source = contextData if contextData is not None else data
        ctx_enc, _ = extractContextMatrix(ctx_source, ctx_feats, detector.context_specs)
        if len(ctx_enc) != len(feat_matrix):
            raise ValueError(
                f"context rows ({len(ctx_enc)}) != telemetry rows "
                f"({len(feat_matrix)}); sources must be row-aligned."
            )
        # detectBatch takes raw (unencoded) contexts; slice per-window raw
        # frames from the source so encoding stays single-sourced.
        raw_ctx_all = extractRawContextColumns(ctx_source, ctx_feats)
        ctx_windows = np.stack(
            [raw_ctx_all[i : i + windowSize] for i in range(len(windows))]
        ) if len(windows) else np.empty((0, windowSize, len(ctx_feats)), dtype=object)

    return detector.detectBatch(windows, ctx_windows, batchSize=batchSize)


def _targetTimestampMs(timestamp: Any) -> int:
    """Normalise a query timestamp to integer ms since the epoch."""
    if isinstance(timestamp, bool):
        raise ValueError("timestamp must not be a boolean")
    if isinstance(timestamp, int) and not isinstance(timestamp, bool):
        return timestamp if timestamp < 10**14 else int(timestamp // 1_000_000)
    if isinstance(timestamp, float):
        return int(timestamp if timestamp < 10**14 else timestamp / 1e6)
    if HAS_PANDAS:
        try:
            return int(pd.to_datetime(timestamp).value // 1_000_000)
        except Exception:
            pass
    raise ValueError(f"Cannot interpret timestamp {timestamp!r}; pass ms int or a datetime")


def inferStateAt(
    detector: ProcessManifoldDetector,
    data: Any,
    timestamp: Any,
    timestampCol: str = "timestamp",
    windowSize: Optional[int] = None,
    contextData: Any = None,
    contextFeatures: Optional[List[str]] = None,
    device: Optional[Union[str, Any]] = None,
) -> Dict[str, Any]:
    """Score the plant state at one wall-clock time: "what was happening then?".

    Takes the ``windowSize`` rows ending at the last row with time <=
    ``timestamp`` and runs :meth:`detect` on them. ``data`` is either a
    DataFrame (needs a datetime ``timestampCol``) or an archive reader with
    ``iterRows`` (frame ms clock, streamed in O(window) memory unless
    ``timestampCol`` names a DTL column). Returns the full verdict plus
    ``window_start_ms`` / ``window_end_ms``.
    """
    if windowSize is None:
        windowSize = detector.window_size
    if device is not None:
        detector.to(device)
    target_ms = _targetTimestampMs(timestamp)
    ctx_feats = contextFeatures if contextFeatures is not None else detector.context_features

    if hasattr(data, "iterRows"):
        from collections import deque

        cols = list(detector.features) + [c for c in ctx_feats if c not in detector.features]
        if timestampCol != "timestamp" and timestampCol not in cols:
            cols = cols + [timestampCol]
        ring: deque = deque(maxlen=windowSize)
        for frame_ts, row in data.iterRows(cols):
            ts_ms = _rowTimestampMs(frame_ts, row.get(timestampCol) if timestampCol else None)
            if ts_ms > target_ms:
                break
            ring.append((ts_ms, row))
        if len(ring) < windowSize:
            raise ValueError(
                f"Only {len(ring)} rows at or before {timestamp!r}; need {windowSize}.")
        ts_list = [t for t, _ in ring]
        cont = np.array([[r.get(f, np.nan) for f in detector.features] for _, r in ring],
                        dtype=np.float32)
        ctx = None
        if ctx_feats:
            ctx_source = contextData if contextData is not None else None
            if ctx_source is not None:
                raise ValueError("contextData with a reader source is unsupported; "
                                 "columns must come from the archive itself.")
            ctx = np.empty((windowSize, len(ctx_feats)), dtype=object)
            for i, (_, r) in enumerate(ring):
                ctx[i, :] = [r.get(f) for f in ctx_feats]
    else:
        if not HAS_PANDAS or not isinstance(data, pd.DataFrame):
            raise TypeError("data must be a DataFrame or an archive reader with iterRows()")
        if timestampCol not in data.columns:
            raise KeyError(f"Timestamp column '{timestampCol}' not in data")
        ts_ms = (pd.to_datetime(data[timestampCol]).astype("datetime64[ns]").astype("int64")
                 // 1_000_000).to_numpy()
        end = int(np.searchsorted(ts_ms, target_ms, side="right"))
        if end < windowSize:
            raise ValueError(
                f"Only {end} rows at or before {timestamp!r}; need {windowSize}.")
        cont = extractFeatureMatrix(data.iloc[end - windowSize : end], detector.features)
        ts_list = [int(t) for t in ts_ms[end - windowSize : end]]
        ctx = None
        if ctx_feats:
            ctx_source = contextData if contextData is not None else data
            ctx_all = extractRawContextColumns(ctx_source, ctx_feats)
            if len(ctx_all) != len(data):
                raise ValueError("contextData rows do not align with data rows")
            ctx = ctx_all[end - windowSize : end]

    verdict = detector.detect(cont, ctx)
    verdict["timestamp_ms"] = target_ms
    verdict["window_start_ms"] = ts_list[0]
    verdict["window_end_ms"] = ts_list[-1]
    return verdict


def predictFeature(
    detector: ProcessManifoldDetector,
    window: Any,
    feature: str,
    context: Any = None,
    device: Optional[Union[str, Any]] = None,
) -> Dict[str, Any]:
    """Nowcast one signal from all the others through the learned manifold.

    Masks ``feature`` to its (stratum-)nominal mean, reconstructs, and reads
    back the model's expectation in engineering units with a conformal
    prediction interval (``±sqrt(q) * std`` from the per-signal bound, else
    the global bound). Reports the last-timestep value. This is a
    manifold-constrained estimate, not a trained forecaster: without
    denoising training the network may lean on the bottleneck alone, so
    treat wide intervals as honest uncertainty, not failure.
    """
    if device is not None:
        detector.to(device)
    if feature not in detector.features:
        raise KeyError(f"Feature '{feature}' not in detector features")
    j = detector.features.index(feature)
    raw_window = np.asarray(
        window[detector.features].to_numpy(dtype=np.float32)
        if HAS_PANDAS and isinstance(window, pd.DataFrame)
        else window,
        dtype=np.float32,
    )
    if raw_window.shape != (detector.window_size, len(detector.features)):
        raise ValueError(
            f"Expected window shape ({detector.window_size}, {len(detector.features)}), "
            f"got {raw_window.shape}"
        )
    ctx_window = None
    raw_ctx_rows = None
    if detector.context_dim > 0:
        if context is None:
            raise ValueError(f"Detector expects discrete context {detector.context_features}")
        raw_ctx_rows = (context if isinstance(context, np.ndarray)
                        else extractRawContextColumns(context, detector.context_features))
        if raw_ctx_rows.shape[0] != detector.window_size:
            raise ValueError("Context rows do not match the window length")
        ctx_window, _, _, _ = detector._encodeRawContext(raw_ctx_rows)

    norm_window, _ = detector._stratifiedNorm(raw_window, raw_ctx_rows)
    masked = norm_window.copy()
    masked[:, j] = 0.0  # stratum/global nominal mean in standardised space
    _, _, recon = detector.computeWindowError(masked, ctx_window)

    # Inverse standardisation with the decision-time (last-row) stratum.
    if detector.strata_stats and detector.stratify_fields and raw_ctx_rows is not None:
        import json as _json

        keys = contextRowKeys(np.asarray(raw_ctx_rows, dtype=object),
                              detector.context_features, detector.context_specs,
                              fields=detector.stratify_fields)
        entry = detector.strata_stats.get(_json.dumps(list(keys[-1])))
        if entry is not None:
            mean = np.asarray(entry["mean"], dtype=np.float32)
            std = np.asarray(entry["std"], dtype=np.float32)
        else:
            mean, std = detector.scaler.mean, detector.scaler.std
    else:
        mean, std = detector.scaler.mean, detector.scaler.std
    predicted = float(recon[-1, j] * std[j] + mean[j])
    actual = float(raw_window[-1, j])
    q = (float(detector.q_alpha_per_signal[j])
         if detector.q_alpha_per_signal is not None else float(detector.q_alpha))
    half_width = float(np.sqrt(q) * std[j]) if np.isfinite(q) else float("inf")
    residual = actual - predicted
    return {
        "feature": feature,
        "predicted": predicted,
        "actual": actual,
        "residual": residual,
        "interval": [predicted - half_width, predicted + half_width],
        "inside_interval": bool(abs(residual) <= half_width),
        "surprise": float(abs(residual) / half_width) if np.isfinite(half_width) and half_width > 0 else 0.0,
        "q_used": q,
    }


def _rowTimestampMs(frame_ts_ms: int, col_value: Any) -> int:
    """Effective row time: DTL/timestamp column wins, else the frame clock."""
    if col_value is None:
        return int(frame_ts_ms)
    if isinstance(col_value, bool):
        return int(frame_ts_ms)
    if isinstance(col_value, int):
        return int(col_value // 1_000_000) if col_value > 10**14 else int(col_value)
    if isinstance(col_value, float):
        return int(col_value if col_value < 10**14 else col_value / 1e6)
    if HAS_PANDAS:
        try:
            return int(pd.to_datetime(col_value).value // 1_000_000)
        except Exception:
            pass
    return int(frame_ts_ms)


def fitManifoldPipelineStream(
    reader: Any,
    features: Optional[List[str]] = None,
    schemaSource: Optional[Union[str, Path, Dict[str, Any]]] = None,
    windowSize: int = 12,
    stride: int = 1,
    faultOnset: Optional[Any] = None,
    trainRatio: float = 0.70,
    timestampCol: Optional[str] = None,
    modelConfig: Optional[dict] = None,
    device: Optional[Union[str, Any]] = None,
    seed: int = 42,
    calibrationStride: Optional[int] = None,
    calibration_stride: Optional[int] = None,
    includeSubsystems: Optional[List[str]] = None,
    excludeSubsystems: Optional[List[str]] = None,
    includeDimensions: Optional[List[str]] = None,
    excludeSignals: Optional[List[str]] = None,
    useContext: bool = True,
    contextFeatures: Optional[List[str]] = None,
    maxContextCardinality: int = 32,
    stratifyOn: Optional[Any] = None,
    minStratumSamples: int = 30,
    trainModeHead: bool = True,
    modeHeadConfig: Optional[Dict[str, Any]] = None,
    shuffleBuffer: int = 2048,
    trainBatchSize: int = 32,
) -> Dict[str, Any]:
    """Train a manifold detector by streaming archive rows — O(window) memory.

    Same model, thresholds and return shape as :func:`fitManifoldPipeline`,
    but rows flow from ``reader.iterRows`` (``BinaryDatasetReader`` /
    ``JsonlDatasetReader``) instead of materialising the (N, F) matrix and
    (N, W, F) windows. Four chronological passes over the archive:

    - A: count rows + resolve the fault cutoff (index or first ts >= onset).
    - B: Welford scaler/strata/category statistics over nominal rows only.
    - C (x epochs): ring-buffer windows -> shuffle buffer -> AE + mode-head
      optimiser steps. One full re-read per epoch is the honest price of
      multi-epoch streaming; the batch path stays for small data.
    - D: single scoring pass over calibration windows for the quantiles.

    ``schemaSource`` defaults to ``reader.schema_json`` (embedded schema) —
    the dataset trains the model directly. Cycle filtering is intentionally
    unsupported here (archives stream in stored order); filter upstream or
    use the batch path. ``splits`` in the return carries counts, not window
    arrays (they were never materialised).
    """
    from .manifold import HAS_TORCH

    if not HAS_TORCH:
        raise ImportError("PyTorch is required to train manifold detectors.")
    import torch

    if modelConfig is None:
        modelConfig = {}
    resolved_schema = schemaSource
    if resolved_schema is None:
        embedded = getattr(reader, "schema_json", None)
        if embedded:
            resolved_schema = embedded
    feature_dimensions = feature_subsystems = None
    schema_context_specs: Dict[str, Any] = {}
    schema_timestamp_fields: List[str] = []
    if resolved_schema is not None:
        schema_cfg = configureModelFromSchema(
            schemaSource=resolved_schema,
            windowSize=windowSize,
            includeSubsystems=includeSubsystems,
            excludeSubsystems=excludeSubsystems,
            includeDimensions=includeDimensions,
            excludeSignals=excludeSignals,
        )
        if features is None:
            features = schema_cfg["features"]
        feature_dimensions = schema_cfg["featureDimensions"]
        feature_subsystems = schema_cfg["featureSubsystems"]
        schema_context_specs = dict(schema_cfg.get("contextSpecs", {}))
        schema_timestamp_fields = list(schema_cfg.get("timestampFields", []))
        if contextFeatures is None:
            contextFeatures = list(schema_cfg.get("contextFeatures", []))
        if "model" not in modelConfig:
            modelConfig["model"] = schema_cfg["modelConfig"]
    if features is None:
        raise ValueError("Either 'features' list, 'schemaSource' or a reader with embedded schema is required.")
    if not useContext:
        contextFeatures = []
    elif contextFeatures is None:
        contextFeatures = []
    context_specs: Dict[str, Any] = {}
    stratify_fields: List[str] = []
    if stratifyOn is False:
        stratify_fields = []
    elif isinstance(stratifyOn, list):
        stratify_fields = [f for f in stratifyOn if f in (contextFeatures or [])]

    columns = list(features) + [c for c in (contextFeatures or []) if c not in features]
    ts_col_requested = timestampCol
    if timestampCol is None and schema_timestamp_fields:
        pass  # resolved per-row below: first present timestamp field wins
    if timestampCol is not None and timestampCol not in columns:
        columns = columns + [timestampCol]

    import numbers as _numbers

    def _fault_cutoff(n_total: int, ts_arr: List[int]) -> int:
        if isinstance(faultOnset, bool):
            return n_total
        if isinstance(faultOnset, _numbers.Integral) and not isinstance(faultOnset, bool):
            return min(n_total, int(faultOnset))
        try:
            import numpy as _np

            if isinstance(faultOnset, _np.integer):
                return min(n_total, int(faultOnset))
        except Exception:
            pass
        if faultOnset is not None and ts_arr:
            try:
                cutoff_ms = int(pd.to_datetime(faultOnset).value // 1_000_000) if HAS_PANDAS else int(faultOnset)
            except Exception:
                return n_total
            for i, t in enumerate(ts_arr):
                if t >= cutoff_ms:
                    return i
        return n_total

    # ---- Pass A: count + timestamps + cutoff --------------------------------
    n_total = 0
    ts_all: List[int] = []
    for ts, _row in reader.iterRows(columns):
        n_total += 1
        ts_all.append(_rowTimestampMs(ts, _row.get(timestampCol) if timestampCol else None))
    if n_total == 0:
        raise ValueError("Archive yielded no decodable rows.")
    cutoff = _fault_cutoff(n_total, ts_all)
    n_pre = cutoff
    n_train = int(n_pre * trainRatio)

    # ---- Pass B: statistics over nominal rows only ---------------------------
    F = len(features)
    C = len(contextFeatures or [])
    global_stats = StreamingStats(F)
    strata: Dict[str, StreamingStats] = {}
    cat_sets: Dict[str, set] = {f: set() for f in (contextFeatures or [])}
    seen_modes: set = set()
    strat_fields_resolved = list(stratify_fields)
    if not strat_fields_resolved and stratifyOn is not False and (contextFeatures or []):
        # Default after specs are known (see below); placeholder resolved post-specs.
        strat_fields_resolved = []
    specs_known = dict(schema_context_specs)
    for _i, (_ts, _row) in enumerate(reader.iterRows(columns)):
        if _i >= n_train:
            break
        cont = np.array([_row.get(f, np.nan) for f in features], dtype=np.float64)
        finite = np.isfinite(cont)
        cont = np.where(finite, cont, 0.0)
        global_stats.update(np.where(finite, cont, np.nan))
        for f in (contextFeatures or []):
            v = _row.get(f)
            if v is not None and not (isinstance(v, float) and np.isnan(v)):
                s = cat_sets[f]
                if len(s) < 4 * maxContextCardinality + 8:
                    s.add(v if isinstance(v, (bool, int, float, str)) else str(v))
    # Specs for explicitly requested but schema-unknown fields.
    missing = [f for f in (contextFeatures or []) if f not in specs_known]
    if missing:
        from .transform import inferContextSpecs as _infer

        cols = {f: sorted(cat_sets[f], key=repr) for f in missing}
        fake = np.empty((max(1, 0), len(missing)), dtype=object)
        # inferContextSpecs expects (n, k) raw columns; feed observed sets.
        widest = max([len(cols[f]) for f in missing] or [0])
        if widest == 0:
            raise ValueError(f"No nominal values observed for context {missing}")
        mat = np.empty((widest, len(missing)), dtype=object)
        for j, f in enumerate(missing):
            vals = list(cols[f])[:widest]
            mat[: len(vals), j] = vals
        inferred = _infer(mat, missing, maxCardinality=maxContextCardinality)
    else:
        inferred = {}
    context_specs = {**{f: specs_known[f] for f in (contextFeatures or []) if f in specs_known}, **inferred}
    if stratifyOn is False:
        stratify_fields = []
    elif isinstance(stratifyOn, list):
        stratify_fields = [f for f in stratifyOn if f in (contextFeatures or [])]
    else:
        stratify_fields = list(contextFeatures or [])
    # Strata accumulators now that specs exist.
    from .transform import contextRowKeys as _keys

    def _rowKeys(raw_vals: List[Any]) -> tuple:
        arr = np.empty((1, len(contextFeatures or [])), dtype=object)
        arr[0, :] = raw_vals
        return _keys(arr, contextFeatures or [], context_specs, fields=stratify_fields or None)[0]

    # Second half of pass B: strata means need keys per row — re-iterate train region.
    strata_acc: Dict[str, StreamingStats] = {}
    for _i, (_ts, _row) in enumerate(reader.iterRows(columns)):
        if _i >= n_train:
            break
        if not stratify_fields:
            break
        raw_vals = [_row.get(f) for f in (contextFeatures or [])]
        key = _rowKeys(raw_vals)
        import json as _json

        jk = _json.dumps(list(key))
        acc = strata_acc.get(jk)
        if acc is None:
            acc = strata_acc[jk] = StreamingStats(F)
        cont = np.array([_row.get(f, np.nan) for f in features], dtype=np.float64)
        acc.update(cont)
    strata_stats: Dict[str, Any] = {}
    for jk, acc in strata_acc.items():
        if int(acc.counts.max(initial=0)) >= minStratumSamples:
            strata_stats[jk] = {"mean": acc.mean.tolist(), "std": acc.std.tolist(),
                                "n": int(acc.counts.max(initial=0))}
    if stratify_fields and not strata_stats:
        print(f"[stratify] No stratum reached {minStratumSamples} nominal rows; using global scaling")
        stratify_fields = []
    elif strata_stats:
        print(f"[stratify] {len(strata_stats)} mode strata over {stratify_fields}")
    if stratify_fields:
        seen_key_sets = set()
        for _i, (_ts, _row) in enumerate(reader.iterRows(columns)):
            if _i >= n_train:
                break
            raw_vals = [_row.get(f) for f in (contextFeatures or [])]
            seen_key_sets.add(_rowKeys(raw_vals))

    scaler = global_stats.toScaler(features)
    if n_train == 0:
        raise ValueError("Empty nominal training region (faultOnset precedes all rows?).")

    # ---- Model ---------------------------------------------------------------
    m_cfg = modelConfig.get("model", {})
    epochs = m_cfg.get("epochs", 50)
    batch_size = trainBatchSize
    lr = m_cfg.get("learning_rate", 0.001)
    hidden_dim = m_cfg.get("hidden_dim", 64)
    latent_dim = m_cfg.get("latent_dim", 16)
    alpha = m_cfg.get("alpha", 0.05)
    k_mult = m_cfg.get("k_multiplier", 1.4)
    torch.manual_seed(seed)
    np.random.seed(seed)
    target_device = resolveDevice(device)
    from .manifold import WindowAutoencoder

    ctx_layout = contextLayout(contextFeatures or [], context_specs)
    ctx_dim = sum(e["width"] for e in ctx_layout) if ctx_layout else 0
    cont_dim = windowSize * F
    model = WindowAutoencoder(input_dim=cont_dim + (windowSize * ctx_dim if ctx_dim else 0),
                              hidden_dim=hidden_dim, latent_dim=latent_dim,
                              context_dim=(windowSize * ctx_dim if ctx_dim else 0),
                              cont_dim=cont_dim)
    model.to(target_device)
    optimizer = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=1e-5)
    criterion = torch.nn.MSELoss()
    mh_cfg = dict(modeHeadConfig or {})
    mode_model = None
    if trainModeHead and ctx_dim:
        mode_model = ModePredictor(cont_dim=cont_dim, layout=ctx_layout,
                                   hidden_dim=int(mh_cfg.get("hidden_dim", 32)))
        mode_model.to(target_device)
    mode_opt = torch.optim.Adam(mode_model.parameters(), lr=float(mh_cfg.get("learning_rate", 1e-3))) if mode_model is not None else None
    bce = torch.nn.BCEWithLogitsLoss(reduction="none")
    ce = torch.nn.CrossEntropyLoss(reduction="none", ignore_index=-1)
    is_bool_head = {f"h{h}": (e["kind"] == "bool") for h, e in enumerate(ctx_layout)}

    # Row decode helpers (closure over resolved specs/scaler/strata).
    last_good = [0.0] * F

    def _decodeRow(row: Dict[str, Any]):
        cont = []
        for j, f in enumerate(features):
            v = row.get(f)
            try:
                fv = float(v)
                fv = fv if np.isfinite(fv) else last_good[j]
            except (TypeError, ValueError):
                fv = last_good[j]
            last_good[j] = fv
            cont.append(fv)
        raw_vals = [row.get(f) for f in (contextFeatures or [])]
        return cont, raw_vals

    def _scaleRows(cont_rows: List[List[float]], ctx_raw_rows: List[List[Any]]):
        import json as _json

        arr = np.asarray(cont_rows, dtype=np.float32)
        if strata_stats and stratify_fields:
            from .transform import applyStratifiedStats as _apply

            keys = _keys(np.asarray(ctx_raw_rows, dtype=object), contextFeatures or [],
                         context_specs, fields=stratify_fields)
            scaled, _ = _apply(arr, keys, strata_stats,
                               {"mean": scaler.mean.tolist(), "std": scaler.std.tolist()})
            return np.asarray(scaled, dtype=np.float32)
        return scaler.transform(arr)

    def _encodeCtxRows(ctx_raw_rows: List[List[Any]]):
        from .transform import encodeContextMatrix as _enc

        if not (contextFeatures or []):
            return None
        enc, _ = _enc(np.asarray(ctx_raw_rows, dtype=object), contextFeatures or [], context_specs)
        return np.asarray(enc, dtype=np.float32)

    # ---- Passes C (train x epochs): ring windows -> shuffle buffer -> steps --
    from collections import deque

    def _rowKeys(raw_vals: List[Any]) -> tuple:
        arr = np.empty((1, len(contextFeatures or [])), dtype=object)
        if contextFeatures:
            arr[0, :] = list(raw_vals)[: len(contextFeatures)]
        return contextRowKeys(arr, contextFeatures or [], context_specs,
                              fields=stratify_fields or None)[0]

    def _trainStep(buf: List[tuple], batch_size: int) -> None:
        import random as _random

        _random.shuffle(buf)
        for s in range(0, len(buf), batch_size):
            chunk = buf[s : s + batch_size]
            xc = torch.from_numpy(np.stack([c[0] for c in chunk])).float()
            parts = [xc] + ([torch.from_numpy(np.stack([c[1] for c in chunk])).float()]
                            if chunk[0][1] is not None else [])
            xb = torch.cat(parts, dim=1).to(target_device)
            yb = xb[:, :cont_dim]
            optimizer.zero_grad()
            recon = model(xb)
            loss = criterion(recon, yb)
            if mode_model is not None and mode_opt is not None:
                outs = mode_model(xb[:, :cont_dim])
                mlosses = []
                for h, entry in enumerate(ctx_layout):
                    key = f"h{h}"
                    logits = outs[key]
                    tb = torch.from_numpy(np.stack([c[2][key] for c in chunk])).to(target_device)
                    vb = torch.from_numpy(np.stack([c[3][key] for c in chunk])).to(target_device)
                    if entry["kind"] == "bool":
                        mlosses.append(bce(logits[:, 0], tb.float().reshape(-1)).mean())
                    else:
                        te = tb.long().reshape(-1)
                        row = ce(logits, te)
                        m = vb.reshape(-1)
                        row = torch.where(m, row, torch.zeros_like(row))
                        mlosses.append(row.sum() / m.sum().clamp_min(1))
                loss = loss + torch.stack(mlosses).mean()
                mode_opt.zero_grad()
            loss.backward()
            optimizer.step()
            if mode_opt is not None:
                mode_opt.step()

    cal_stride = calibrationStride if calibrationStride is not None else calibration_stride
    eff_cal_stride = max(1, int(cal_stride if cal_stride is not None else windowSize))
    model.train()
    if mode_model is not None:
        mode_model.train()
    for _ep in range(max(0, epochs)):
        ring_c: deque = deque(maxlen=windowSize)
        ring_x: deque = deque(maxlen=windowSize)
        buf: List[tuple] = []
        row_idx = -1
        for _ts, _row in reader.iterRows(columns):
            row_idx += 1
            if row_idx >= cutoff:
                break
            cont, raw_vals = _decodeRow(_row)
            ring_c.append(cont)
            ring_x.append(raw_vals)
            if len(ring_c) < windowSize:
                continue
            start = row_idx - windowSize + 1
            if (start % stride) != 0:
                continue
            if row_idx >= n_train:
                continue  # calibration/test regions handled in pass D
            w_cont = _scaleRows(list(ring_c), list(ring_x))
            w_ctx = _encodeCtxRows(list(ring_x))
            flat_c = np.asarray(w_cont, dtype=np.float32).reshape(-1)
            flat_x = (np.asarray(w_ctx, dtype=np.float32).reshape(-1)
                      if w_ctx is not None else None)
            mode_t, mode_v = {}, {}
            if mode_model is not None and w_ctx is not None:
                enc_last = np.asarray(w_ctx, dtype=np.float32)[-1:].reshape(1, -1)
                mode_t, mode_v = _modeTargets(enc_last, ctx_layout)
                mode_t = {k: np.asarray(v) for k, v in mode_t.items()}
                mode_v = {k: np.asarray(v) for k, v in mode_v.items()}
            buf.append((flat_c, flat_x, mode_t, mode_v))
            if len(buf) >= shuffleBuffer:
                _trainStep(buf, trainBatchSize)
                buf = []
        if buf:
            _trainStep(buf, trainBatchSize)

    # ---- Pass D: calibration scores ------------------------------------------
    cal_scores: List[float] = []
    cal_sig: List[List[float]] = []
    cal_pos = -1
    scorer = ProcessManifoldDetector(
        model=model, scaler=scaler, features=features, window_size=windowSize,
        q_alpha=float("inf"), device=target_device,
        feature_dimensions=feature_dimensions, feature_subsystems=feature_subsystems,
        context_features=contextFeatures or None, context_specs=context_specs or None,
    )
    ring_c = deque(maxlen=windowSize)
    ring_x = deque(maxlen=windowSize)
    row_idx = -1
    for _ts, _row in reader.iterRows(columns):
        row_idx += 1
        if row_idx >= cutoff:
            break
        cont, raw_vals = _decodeRow(_row)
        ring_c.append(cont)
        ring_x.append(raw_vals)
        if len(ring_c) < windowSize:
            continue
        start = row_idx - windowSize + 1
        if (start % stride) != 0:
            continue
        end = row_idx
        if end < n_train:
            continue
        cal_pos += 1
        if (cal_pos % eff_cal_stride) != 0:
            continue
        w_cont = _scaleRows(list(ring_c), list(ring_x))
        w_ctx = _encodeCtxRows(list(ring_x))
        s, per_sig, _ = scorer.computeWindowError(w_cont, w_ctx)
        cal_scores.append(s)
        cal_sig.append([float(v) for v in per_sig])
    q_alpha = _conformalQuantile(cal_scores, alpha)
    q_per_signal = [
        _conformalQuantile([row[j] for row in cal_sig], alpha) for j in range(F)
    ] if cal_sig else [float("inf")] * F
    n_cal_effective = len(cal_scores)
    print(f"[conformal] Streaming calibration windows: {n_cal_effective} "
          f"(stride {eff_cal_stride}); q={q_alpha:.5f}")

    # Mode-head bounds on the same calibration pass would need a second read;
    # reuse pass-D windows is impossible post-hoc, so score them streaming too.
    q_mode: Dict[str, float] = {}
    seen_modes: Optional[List[List[str]]] = None
    if mode_model is not None:
        from .manifold import modeNLLs as _mnll

        mode_cals: Dict[str, List[float]] = {f: [] for f in (contextFeatures or [])}
        ring_c = deque(maxlen=windowSize)
        ring_x = deque(maxlen=windowSize)
        row_idx = -1
        cal_pos = -1
        for _ts, _row in reader.iterRows(columns):
            row_idx += 1
            if row_idx >= cutoff:
                break
            cont, raw_vals = _decodeRow(_row)
            ring_c.append(cont)
            ring_x.append(raw_vals)
            if len(ring_c) < windowSize:
                continue
            start = row_idx - windowSize + 1
            if (start % stride) != 0:
                continue
            if row_idx < n_train:
                continue
            cal_pos += 1
            if (cal_pos % eff_cal_stride) != 0:
                continue
            w_cont = _scaleRows(list(ring_c), list(ring_x))
            w_ctx = _encodeCtxRows(list(ring_x))
            nlls = _mnll(mode_model, w_cont.reshape(1, windowSize, F),
                         w_ctx.reshape(1, windowSize, ctx_dim),
                         ctx_layout, target_device)
            for f, vals in nlls.items():
                mode_cals[f].extend(vals)
        for f, vals in mode_cals.items():
            q_mode[f] = _conformalQuantile([v for v in vals if np.isfinite(v)], alpha)
        # Nominal modes from a bounded train-region scan (first 50k rows cap).
        seen: set = set()
        for _i, (_ts, _row) in enumerate(reader.iterRows(columns)):
            if _i >= n_train or len(seen) > 50000:
                break
            raw_vals = [_row.get(f) for f in (contextFeatures or [])]
            seen.add(_rowKeys(raw_vals))
        seen_modes = sorted([list(m) for m in seen]) if seen else None

    detector = ProcessManifoldDetector(
        model=model,
        scaler=scaler,
        features=features,
        window_size=windowSize,
        q_alpha=q_alpha,
        k_multiplier=k_mult,
        alpha=alpha,
        device=target_device,
        feature_dimensions=feature_dimensions,
        feature_subsystems=feature_subsystems,
        q_alpha_per_signal=q_per_signal,
        calibration_stride=eff_cal_stride,
        context_features=contextFeatures or None,
        context_specs=context_specs or None,
        seen_modes=seen_modes,
        stratify_fields=stratify_fields or None,
        strata_stats=strata_stats or None,
        mode_model=mode_model,
        q_mode_per_field=q_mode,
    )
    splits = {
        "trainRaw": None, "calRaw": None, "testRaw": None,
        "n_total": n_total, "n_train_rows": n_train, "cutoff": cutoff,
        "nCalEffective": n_cal_effective,
        "train_raw": None, "cal_raw": None, "test_raw": None,
        "n_cal_effective": n_cal_effective,
    }
    train_config_stream = {
        "epochs": epochs,
        "seed": seed,
        "alpha": alpha,
        "window_size": windowSize,
        "stride": stride,
        "calibration_stride": eff_cal_stride,
        "n_train_rows": n_train,
        "n_cal_effective": n_cal_effective,
        "n_features": len(features),
        "n_context": len(contextFeatures or []),
        "streaming": True,
    }
    return {
        "detector": detector,
        "scaler": scaler,
        "splits": splits,
        "device": str(detector.device),
        "features": features,
        "windowSize": windowSize,
        "featureDimensions": feature_dimensions,
        "featureSubsystems": feature_subsystems,
        "contextFeatures": list(contextFeatures or []),
        "contextSpecs": dict(context_specs),
        "timestampFields": list(schema_timestamp_fields),
        "stratifyFields": list(stratify_fields),
        "strata": {k: v.get("n", 0) for k, v in strata_stats.items()},
        "trainConfig": train_config_stream,
        "calibrationStride": eff_cal_stride,
        "nCalEffective": n_cal_effective,
        "window_size": windowSize,
        "feature_dimensions": feature_dimensions,
        "feature_subsystems": feature_subsystems,
        "context_features": list(contextFeatures or []),
        "context_specs": dict(context_specs),
        "stratify_fields": list(stratify_fields),
        "train_config": train_config_stream,
        "calibration_stride": eff_cal_stride,
        "n_cal_effective": n_cal_effective,
    }


# Backward-compatible snake_case aliases
fit_manifold_pipeline = fitManifoldPipeline
fit_manifold_pipeline_stream = fitManifoldPipelineStream
infer_state_at = inferStateAt
predict_feature = predictFeature
evaluate_manifold_sequence = evaluateManifoldSequence
