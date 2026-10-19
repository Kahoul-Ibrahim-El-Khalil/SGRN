#!/usr/bin/env python3
"""Shared evaluation primitives for the FLARE_BLOCKED experiment.

Everything here is deliberately implementation-free of matplotlib/plotting and
free of file IO so both ``evaluate.py`` (standalone) and ``orchestrate.py``
(``flare run``) produce IDENTICAL metrics — there is exactly one place that
computes false-alarm rates, thresholds and baselines.

Statistical content:
  - false-alarm rates on HELD-OUT-NORMAL windows with raw counts,
  - non-overlapping-window rates with a Clopper–Pearson 95% CI,
  - calibrated single-signal baselines (level + slope) whose threshold k is
    chosen ONLY on the calibration split (never on held-out or fault data),
  - provenance (seed, git commit, config hash).
"""

from __future__ import annotations

import hashlib
import math
import random
import subprocess
from pathlib import Path
from typing import Iterable

import numpy as np

try:  # optional heavy deps are only needed for seeding torch
    import torch  # type: ignore
    HAS_TORCH = True
except ImportError:  # pragma: no cover
    HAS_TORCH = False

ROOT = Path(__file__).resolve().parents[2]

BASELINE_LEVEL = "level"
BASELINE_SLOPE = "slope"
DEFAULT_BASELINE_SIGNAL = "SafetySystems.flare_header_press"
DEFAULT_K_GRID = [round(0.5 + 0.25 * i, 4) for i in range(31)]  # 0.5 .. 8.0


# ======================================================================================
# Determinism / provenance (reproducibility)
# ======================================================================================

def configureSeeds(seed: int) -> None:
    """Fix every RNG that can influence this pipeline stage.

    torch/numpy re-seeding also happens inside ``trainManifoldDetector``; this
    helper additionally covers ``random`` (shuffle paths, tie-breaks) and is
    called with the CONFIG seed, never a hardcoded default.
    """
    seed = int(seed)
    random.seed(seed)
    np.random.seed(seed)
    if HAS_TORCH:
        torch.manual_seed(seed)
        if torch.cuda.is_available():
            torch.cuda.manual_seed_all(seed)


def collectProvenance(config: dict, config_path: Path | None = None) -> dict:
    """Record seed, git commit hash and config-file hash for a run.

    ``config_hash`` is the SHA-256 of the config FILE (the resolved config for
    orchestrate runs, the experiment config for standalone evaluation) so any
    drift between runs is detectable.
    """
    try:
        git_commit = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=str(ROOT),
            capture_output=True, text=True, timeout=10).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        git_commit = None
    config_hash = None
    if config_path is not None and Path(config_path).is_file():
        config_hash = hashlib.sha256(Path(config_path).read_bytes()).hexdigest()
    ds_cfg = config.get("dataset", {}) or {}
    return {
        "seed": int(ds_cfg.get("random_seed", 0)) or None,
        "seed_note": "dataset.random_seed; seeds random, numpy and torch",
        "git_commit": git_commit,
        "config_path": str(config_path) if config_path is not None else None,
        "config_sha256": config_hash,
    }


# ======================================================================================
# Binomial stats
# ======================================================================================

def _binomTail(k: int, n: int, p: float, upper: bool) -> float:
    """P(X <= k | p) when ``upper`` else P(X >= k | p) for X ~ Bin(n, p).

    Exact with ``math.comb`` (bisection callers keep p in [0, 1], where the
    power terms stay finite; p = 0/1 handled by the monotonicity callers).
    """
    if p <= 0.0:
        return 1.0 if upper else (1.0 if k >= n else 0.0)
    if p >= 1.0:
        return 0.0 if upper else (1.0 if k <= 0 else 0.0)
    q = 1.0 - p
    total = 0.0
    if upper:
        for i in range(0, k + 1):
            total += math.comb(n, i) * (p ** i) * (q ** (n - i))
    else:
        for i in range(k, n + 1):
            total += math.comb(n, i) * (p ** i) * (q ** (n - i))
    return min(1.0, max(0.0, total))


def clopperPearsonInterval(k: int, n: int, confidence: float = 0.95) -> tuple[float, float]:
    """Exact (Clopper–Pearson) two-sided CI for a binomial proportion.

    Returns ``(lower, upper)`` for ``k`` flaggings out of ``n`` trials.
    ``n == 0`` → ``(0.0, 1.0)``; ``k == 0`` → lower bound 0; ``k == n`` →
    upper bound 1. Bounds solve the standard equal-tail binomial equations:
    ``P(X >= k | p_lo) = (1-conf)/2`` and ``P(X <= k | p_hi) = (1-conf)/2``.
    """
    if n <= 0:
        return (0.0, 1.0)
    if not (0 < k <= n):
        k = 0 if k < 0 else n
    tail = (1.0 - confidence) / 2.0

    def _bisect(f, increasing: bool, lo: float, hi: float, target: float, iters: int = 200) -> float:
        for _ in range(iters):
            mid = (lo + hi) / 2.0
            val = f(mid)
            if increasing == (val < target):
                lo = mid
            else:
                hi = mid
        return (lo + hi) / 2.0

    lower = 0.0 if k == 0 else _bisect(
        lambda p: _binomTail(k, n, p, upper=False), True, 0.0, 1.0, tail)
    upper = 1.0 if k == n else _bisect(
        lambda p: _binomTail(k, n, p, upper=True), False, 0.0, 1.0, tail)
    return (lower, upper)


# ======================================================================================
# False-alarm accounting
# ======================================================================================

def falseAlarmSummary(states: Iterable[str]) -> dict:
    """Raw counts + rates for one set of detector states on normal data.

    ``rate_total`` counts UNCERTAIN or ABNORMAL windows; ``rate_abnormal_only``
    counts only confident ABNORMAL windows. Rate denominators are the window
    count itself — 0 windows produce rates of 0.0 with ``n_windows == 0`` so
    callers can surface the emptiness instead of faking a rate.
    """
    states = list(states)
    n = len(states)
    n_uncertain = sum(1 for s in states if s == "uncertain")
    n_abnormal = sum(1 for s in states if s == "abnormal")
    n_flagged_total = n_uncertain + n_abnormal
    return {
        "n_windows": n,
        "n_flagged_uncertain": n_uncertain,
        "n_flagged_abnormal": n_abnormal,
        "n_flagged_total_uncertain_or_abnormal": n_flagged_total,
        "rate_total": (n_flagged_total / n) if n else 0.0,
        "rate_abnormal_only": (n_abnormal / n) if n else 0.0,
    }


def scoreDetectorWindows(detector, raw_values: np.ndarray, window_size: int,
                         stride: int = 1) -> list:
    """Run ``detector.detect`` over every full window of ``raw_values``.

    Returns the state list ("normal"/"uncertain"/"abnormal"), one entry per
    window. Raw (engineering-unit) windows are passed straight to the detector
    — internal standardisation uses the train-fitted scaler only.
    """
    raw_values = np.asarray(raw_values, dtype=np.float32)
    states = []
    if len(raw_values) < window_size:
        return states
    for i in range(0, len(raw_values) - window_size + 1, max(1, int(stride))):
        res = detector.detect(raw_values[i:i + window_size])
        states.append(res["state"])
    return states


# ======================================================================================
# Single-signal baselines (level + slope), calibrated on the calibration split only
# ======================================================================================

def windowStatisticSeries(raw_values: np.ndarray, col: int, window_size: int,
                          stride: int, statistic: str) -> np.ndarray:
    """Per-window scalar statistic of ONE signal, raw engineering units.

    - ``"level"``: mean of the signal over the window rows.
    - ``"slope"``: per-tick difference of the signal, averaged over the
      window (mean of 11 successive diffs in a 12-row window), i.e. the
      average climb rate carried by the window.
    """
    raw_values = np.asarray(raw_values, dtype=np.float64)
    if len(raw_values) < window_size:
        return np.zeros(0)
    signals = raw_values[:, col]
    offsets = range(0, len(raw_values) - window_size + 1, max(1, int(stride)))
    stats = np.empty(0)
    if statistic == BASELINE_LEVEL:
        stats = np.array([float(np.mean(signals[i:i + window_size])) for i in offsets])
    elif statistic == BASELINE_SLOPE:
        diffs = np.diff(signals)  # diffs[j] = signals[j+1] - signals[j]
        stats = np.array([float(np.mean(diffs[i:i + window_size - 1])) for i in offsets])
    else:
        raise ValueError(f"Unknown baseline statistic {statistic!r}")
    return stats


def calibrateBaselineThreshold(train_stats: np.ndarray, cal_stats: np.ndarray,
                               alpha: float, k_grid: Iterable[float]) -> dict:
    """Pick the SHORTEST k whose calibration-window FAR stays within alpha.

    Threshold = train statistic mean + k * train statistic std, computed from
    TRAIN split windows only. k is then scanned ascending over ``k_grid`` and
    the first k whose calibration FAR <= alpha wins (most sensitive threshold
    the calibration budget allows). Calibration data is used ONLY here — held-
    out and fault-day statistics are never consulted, so no peeking happens.
    If even the largest k exceeds alpha (pathological cal split), the last k
    is returned with ``budget_respected=false``.
    """
    train_stats = np.asarray(train_stats, dtype=np.float64)
    cal_stats = np.asarray(cal_stats, dtype=np.float64)
    mean = float(np.mean(train_stats)) if len(train_stats) else float("nan")
    std = float(np.std(train_stats)) if len(train_stats) else float("nan")
    n_cal = len(cal_stats)
    chosen = None
    for k in k_grid:
        threshold = mean + float(k) * std
        n_flag = int(np.count_nonzero(cal_stats >= threshold)) if n_cal else 0
        rate = (n_flag / n_cal) if n_cal else 0.0
        chosen = {"k": float(k), "threshold": float(threshold),
                  "calibration_n_windows": n_cal, "calibration_n_flagged": n_flag,
                  "calibration_flag_rate": float(rate)}
        if rate <= alpha:
            break
    chosen["budget_respected"] = bool(chosen["calibration_flag_rate"] <= alpha) if n_cal else False
    chosen["mean_std_source"] = "train-split windows only (no held-out, no fault-day rows)"
    return chosen


def thresholdStates(stats: np.ndarray, threshold: float) -> list:
    """Binary states for statistics vs threshold: 'abnormal' | 'normal'."""
    return ["abnormal" if s >= threshold else "normal" for s in np.asarray(stats, dtype=np.float64)]


def baselineBundle(raw_train: np.ndarray, raw_cal: np.ndarray, raw_heldout: np.ndarray,
                   raw_timeline: np.ndarray, feature_index: int, window_size: int,
                   statistic: str, threshold_info: dict,
                   stride: int = 1) -> dict:
    """Assemble FAR numbers for one baseline on held-out-normal data.

    Mirrors :func:`falseAlarmSummary` bundles of the detector: overlapping
    (stride) windows give the primary rate, non-overlapping windows
    (stride = window_size) give the independent-sample rate. (The
    Clopper–Pearson CI is attached by the caller alongside the delay metrics,
    which need timestamps.)
    """
    stats_kwargs = dict(col=feature_index, window_size=window_size, statistic=statistic)
    heldout_overlap = windowStatisticSeries(raw_heldout, stride=stride, **stats_kwargs)
    heldout_indep = windowStatisticSeries(raw_heldout, stride=window_size, **stats_kwargs)
    heldout_summary = falseAlarmSummary(thresholdStates(heldout_overlap, threshold_info["threshold"]))
    heldout_indep_summary = falseAlarmSummary(
        thresholdStates(heldout_indep, threshold_info["threshold"]))
    return {
        "heldout": heldout_summary,
        "heldout_non_overlapping": heldout_indep_summary,
        "k": threshold_info["k"],
        "threshold": threshold_info["threshold"],
        "calibration_flag_rate": threshold_info["calibration_flag_rate"],
        "calibration_n_windows": threshold_info["calibration_n_windows"],
    }
