#!/usr/bin/env python3
"""Train detector autoencoder and compute split-conformal thresholds.

One command:
    python experiments/flare/train_detector.py

Outputs saved to experiments/flare/:
    - model.pt
    - model_meta.json (scaler, feature list, thresholds)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

from dataset import get_splits_and_windows
from model import train_autoencoder


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save-dir", default=str(Path(__file__).parent), help="Directory to save model artifacts")
    parser.add_argument("--seed", type=int, default=42, help="Fixed random seed for torch and numpy")
    args = parser.parse_args()

    save_dir = Path(args.save_dir).resolve()
    print(f"[train] Loading dataset splits from {save_dir}...")
    splits = get_splits_and_windows()

    print(f"[train] Training Autoencoder on {len(splits['X_train'])} training windows...")
    detector = train_autoencoder(
        X_train=splits["X_train"],
        X_cal=splits["X_cal"],
        scaler=splits["scaler"],
        features=splits["features"],
        config=splits["config"],
        seed=args.seed,
    )

    detector.save(save_dir)
    print(f"[train] Detector successfully saved to {save_dir}")

    # Sanity check detect on a normal window and a test window
    normal_sample = splits["cal_raw"][:splits["config"]["dataset"]["window_size"]]
    normal_result = detector.detect(normal_sample)
    print(f"[sanity] Normal sample detection: state={normal_result['state']} score={normal_result['score']:.5f}")

    # Test sample with preceding context to form full window_size
    window_size = splits["config"]["dataset"]["window_size"]
    test_context_raw = np.vstack([splits["cal_raw"][-(window_size - len(splits["test_raw"])):], splits["test_raw"]]) if len(splits["test_raw"]) < window_size else splits["test_raw"][-window_size:]
    test_result = detector.detect(test_context_raw)
    print(f"[sanity] Fault sample detection:  state={test_result['state']} score={test_result['score']:.5f}")
    print(f"         Top signals: {[s['signal'] for s in test_result['top_signals']]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
