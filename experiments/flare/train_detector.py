#!/usr/bin/env python3
"""Train detector autoencoder and compute split-conformal thresholds.

One command:
    python experiments/flare/train_detector.py

Split usage (three-way chronological, see dataset.py):
    - train split       -> autoencoder fit
    - calibration split -> conformal quantile q_alpha ONLY
    - held-out normal   -> NOT used here at all (reserved for FAR evaluation)
    - fault day         -> test

Outputs saved to experiments/flare/:
    - model/model.pt
    - model/model_meta.json (scaler, feature list, thresholds)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

from config_loader import loadConfig, getConfigPath
from dataset import getSplitsAndWindows
from model import train_autoencoder
from evaluation_common import configureSeeds


def _getWindowSize(config: dict) -> int:
    """Extract window_size from config, supporting both legacy int and new dict format."""
    ws = config.get("dataset", {}).get("window_size", 12)
    if isinstance(ws, dict):
        return int(ws.get("size", 12))
    return int(ws)


def buildTestContext(cal_raw: np.ndarray, test_raw: np.ndarray, window_size: int) -> np.ndarray:
    """Prepend up to window_size-1 calibration rows so the first test window is full.

    The naive ``cal[-(window_size - n):]`` slice returns the WHOLE array when
    ``n == window_size`` (``-0``); the explicit branch below keeps exactly
    the needed tail.
    """
    need = max(0, window_size - len(test_raw))
    if need:
        if len(cal_raw) < need:
            raise ValueError(f"Need {need} calibration context rows, have {len(cal_raw)}")
        return np.vstack([cal_raw[-need:], test_raw])
    return test_raw[-window_size:]


def main() -> int:
    config = loadConfig(getConfigPath(Path(__file__).parent))
    config_seed = int((config.get("dataset") or {}).get("random_seed", 42))

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save-dir", default=str(Path(__file__).parent),
                        help="Parent directory receiving a 'model/' subfolder with "
                             "model.pt + model_meta.json (dataset input is unaffected; "
                             "it always comes from the module dataset defaults)")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed for random/numpy/torch "
                             f"(default: dataset.random_seed from config = {config_seed})")
    args = parser.parse_args()
    seed = args.seed if args.seed is not None else config_seed

    save_dir = Path(args.save_dir).resolve()
    model_dir = save_dir / "model"
    model_dir.mkdir(parents=True, exist_ok=True)
    print(f"[train] Dataset splits from module defaults; artifacts will go to {model_dir}...")
    splits = getSplitsAndWindows()
    if len(splits["X_train"]) == 0:
        raise SystemExit("[train] ERROR: no training windows (empty/short nominal history?)")
    if len(splits["X_cal"]) == 0:
        print("[train] WARNING: no calibration windows; thresholds will be infinite (never flag).")

    sr = splits.get("split_rows", {})
    print(f"[train] Splits (rows): train={sr.get('train')} cal={sr.get('calibration')} "
          f"heldout={sr.get('heldout')} test={sr.get('test')} "
          f"(heldout_source={splits.get('heldout_source')} — heldout is NEVER used "
          "for fitting or calibration)")

    print(f"[train] Training Autoencoder on {len(splits['X_train'])} training windows...")
    configureSeeds(seed)  # random + numpy + torch, from config (reproducibility)
    detector = train_autoencoder(
        X_train=splits["X_train"],
        X_cal=splits["X_cal"],
        scaler=splits["scaler"],
        features=splits["features"],
        config=splits["config"],
        seed=seed,
    )

    detector.save(model_dir)
    print(f"[train] Detector successfully saved to {model_dir}")

    # Sanity check detect on a normal window and a test window.
    # Both need at least window_size rows; short/empty splits raise here
    # with context instead of a reshape error inside the model.
    window_size = _getWindowSize(splits["config"])
    if len(splits["cal_raw"]) < window_size:
        raise SystemExit(f"[train] ERROR: calibration history ({len(splits['cal_raw'])} rows) "
                         f"shorter than window_size={window_size}")
    normal_sample = splits["cal_raw"][:window_size]
    normal_result = detector.detect(normal_sample)
    print(f"[sanity] Normal sample detection: state={normal_result['state']} score={normal_result['score']:.5f}")

    # Test sample with preceding context to form full window_size.
    test_raw = splits["test_raw"]
    if len(test_raw) == 0:
        print("[sanity] No test rows; skipping fault sanity check.")
    else:
        test_context_raw = buildTestContext(splits["cal_raw"], test_raw, window_size)
        test_result = detector.detect(test_context_raw)
        print(f"[sanity] Fault sample detection:  state={test_result['state']} score={test_result['score']:.5f}")
        print(f"         Top signals: {[s['signal'] for s in test_result['top_signals']]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
