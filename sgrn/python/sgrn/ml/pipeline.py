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

from .device import resolveDevice
from .manifold import ProcessManifoldDetector, trainManifoldDetector
from .schema import configureModelFromSchema
from .transform import (
    Scaler,
    createSlidingWindows,
    extractFeatureMatrix,
    filterProcessTimeline,
    fitProcessScaler,
    splitProcessTimeline,
)


def fitManifoldPipeline(
    data: Any,
    features: Optional[List[str]] = None,
    schemaSource: Optional[Union[str, Path]] = None,
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
) -> Dict[str, Any]:
    """Composable end-to-end functional pipeline that fits a Manifold Anomaly Detector.
    
    If `schemaSource` is provided and `features` is None, features and their physical
    dimensions are automatically discovered and configured directly from the SCL schema!
    
    Returns:
      - 'detector': Trained ProcessManifoldDetector
      - 'scaler': Fitted Scaler
      - 'splits': Dictionary of data splits
      - 'device': Active compute device string
      - 'features': List of signal names
      - 'featureDimensions': Physical dimensions per signal
      - 'featureSubsystems': Plant subsystem per signal
    """
    if modelConfig is None:
        modelConfig = {}

    feature_dimensions = None
    feature_subsystems = None

    # Step 0: Auto-configure features and dimensions from SCL schema if provided
    if schemaSource is not None:
        schema_cfg = configureModelFromSchema(
            schemaSource=schemaSource,
            windowSize=windowSize,
        )
        if features is None:
            features = schema_cfg["features"]
        feature_dimensions = schema_cfg["featureDimensions"]
        feature_subsystems = schema_cfg["featureSubsystems"]
        if "model" not in modelConfig:
            modelConfig["model"] = schema_cfg["modelConfig"]

    if features is None:
        raise ValueError("Either 'features' list or 'schemaSource' path must be provided.")

    # Step 1: Filter dataset by process cycle boundaries
    filtered_data, timestamps = filterProcessTimeline(
        data=data,
        startTime=cycleStart,
        endTime=cycleEnd,
        timestampCol=timestampCol,
    )

    # Step 2: Extract clean numeric feature matrix
    feat_matrix = extractFeatureMatrix(filtered_data, features)

    # Step 3: Chronological partition into nominal (train, cal) and test
    splits = splitProcessTimeline(
        data=feat_matrix,
        trainRatio=trainRatio,
        calRatio=calRatio,
        timestamps=timestamps,
        faultOnsetTime=(
            faultOnset
            if isinstance(faultOnset, (str, pd.Timestamp) if HAS_PANDAS else str)
            else None
        ),
        faultOnsetIdx=faultOnset if isinstance(faultOnset, int) else None,
    )

    # Step 4: Fit scaler strictly on nominal training telemetry
    scaler = fitProcessScaler(splits["trainRaw"], features)

    # Step 5: Standardise splits and create sliding windows
    train_norm = scaler.transform(splits["trainRaw"])
    cal_norm = scaler.transform(splits["calRaw"])

    X_train = createSlidingWindows(train_norm, windowSize=windowSize, stride=stride)
    X_cal = createSlidingWindows(cal_norm, windowSize=windowSize, stride=stride)

    splits["X_train"] = X_train
    splits["X_cal"] = X_cal

    if len(splits["testRaw"]) > 0:
        test_norm = scaler.transform(splits["testRaw"])
        test_context = np.vstack([cal_norm[-(windowSize - 1) :], test_norm])
        splits["X_test"] = createSlidingWindows(test_context, windowSize=windowSize, stride=stride)
    else:
        splits["X_test"] = np.empty((0, windowSize, len(features)), dtype=np.float32)

    # Step 6: Train autoencoder and calibrate split-conformal bounds (accelerated on GPU/ROCm)
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
    )

    return {
        "detector": detector,
        "scaler": scaler,
        "splits": splits,
        "device": str(detector.device),
        "features": features,
        "windowSize": windowSize,
        "featureDimensions": feature_dimensions,
        "featureSubsystems": feature_subsystems,
        # snake_case backward-compatibility keys
        "window_size": windowSize,
        "feature_dimensions": feature_dimensions,
        "feature_subsystems": feature_subsystems,
    }


def evaluateManifoldSequence(
    detector: ProcessManifoldDetector,
    data: Any,
    features: Optional[List[str]] = None,
    windowSize: Optional[int] = None,
    batchSize: int = 256,
    device: Optional[Union[str, Any]] = None,
) -> Dict[str, Any]:
    """Evaluate an entire continuous dataset sequence with GPU-accelerated batch scoring."""
    if features is None:
        features = detector.features
    if windowSize is None:
        windowSize = detector.window_size

    if device is not None:
        detector.to(device)

    feat_matrix = extractFeatureMatrix(data, features)
    windows = createSlidingWindows(feat_matrix, windowSize=windowSize, stride=1)

    return detector.detectBatch(windows, batchSize=batchSize)


# Backward-compatible snake_case aliases
fit_manifold_pipeline = fitManifoldPipeline
evaluate_manifold_sequence = evaluateManifoldSequence
