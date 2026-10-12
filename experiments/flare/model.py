#!/usr/bin/env python3
"""Glue module exposing sgrn.ml.manifold components for the Flare Blocked experiment.

The core PyTorch autoencoder, split-conformal calibration, 3-state uncertainty
classification, and directional residual attribution logic have been abstracted
into the primary `sgrn.ml` package (`sgrn.ml.manifold`).

This module serves as lightweight experiment glue code to maintain seamless
compatibility with `train_detector.py`, `evaluate.py`, and `run_demo.py`.
"""

from __future__ import annotations

from sgrn.ml.manifold import (
    ProcessManifoldDetector as FlareDetector,
    Scaler,
    WindowAutoencoder,
    train_manifold_detector as train_autoencoder,
)

__all__ = [
    "FlareDetector",
    "WindowAutoencoder",
    "train_autoencoder",
    "Scaler",
]
