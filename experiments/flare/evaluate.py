#!/usr/bin/env python3
"""Evaluation script for the Flare Blocked anomaly detection prototype.

Calculates and prints — from ONE code path shared with orchestrate.py:

  - False alarm rate on HELD-OUT-NORMAL windows (never on the calibration
    split that sets q_alpha — a (1-alpha) quantile exceeds on its own
    calibration set by construction), with raw counts (n_windows, n_flagged).
  - The same rate on NON-OVERLAPPING windows (stride = window size) plus a
    Clopper–Pearson 95% confidence interval for the abnormal-only FAR, so the
    small effective sample stays visible.
  - Detection delay / lead time before ESD.
  - Attribution check at detection (ranking, not root cause).
  - Baselines: calibrated single-signal level and slope thresholds on
    SafetySystems.flare_header_press, with k chosen on the calibration split
    only (FAR <= alpha there), then evaluated on the SAME held-out normal and
    fault-day data as the autoencoder.
  - Calibration-split rates, kept under `calibration_rate_*` keys with an
    explicit note that they are NOT estimates of generalization.
  - Provenance: random seed, git commit, config hash.

Generates evaluation_plot.png (score vs time with thresholds, fault onset,
ESD marker, and first detections of the detector vs baselines).

One command:
  python experiments/flare/evaluate.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "sgrn/python") not in sys.path:
    sys.path.insert(0, str(ROOT / "sgrn/python"))

from config_loader import loadConfig, getConfigPath, getWindowRows
from dataset import getSplitsAndWindows, resolveModelDir
from model import FlareDetector
from evaluation_common import (
    BASELINE_LEVEL,
    BASELINE_SLOPE,
    DEFAULT_BASELINE_SIGNAL,
    DEFAULT_K_GRID,
    calibrateBaselineThreshold,
    clopperPearsonInterval,
    collectProvenance,
    falseAlarmSummary,
    scoreDetectorWindows,
    thresholdStates,
    windowStatisticSeries,
)


FLARE_DIR = Path(__file__).parent
CONFIG_PATH = getConfigPath(FLARE_DIR)
META_PATH = FLARE_DIR / "meta.json"
ARCHIVE = FLARE_DIR / "flare_history.jsonl.zst"
DEFAULT_MODEL_DIR = FLARE_DIR / "model"


def epochMsUtc(dt) -> int:
    """Naive datetimes are UTC instants (gateway frame clock is ms UTC epoch).

    Never use naive ``.timestamp()`` here: it interprets wall time in the
    HOST timezone and skews delays by the UTC offset on non-UTC machines.
    """
    import calendar

    tt = pd.to_datetime(dt)
    if getattr(tt, "tzinfo", None) is not None:
        tt = tt.tz_convert("UTC").tz_localize(None)
    return calendar.timegm(tt.timetuple()) * 1000 + int(tt.microsecond / 1000)


def parseEvaluationTimestamps(meta: dict):
    """Extract and validate (fault_onset, esd|None) datetimes from experiment metadata.

    Raises ValueError with an actionable message on missing or unparseable
    timestamps instead of a bare KeyError mid-evaluation.
    """
    for key in ("fault_onset_iso", "fault_onset_ts"):
        if key not in meta:
            raise ValueError(f"Experiment metadata is missing required key '{key}'")
    try:
        _dt = pd.to_datetime(meta["fault_onset_iso"])
        fault_onset_dt = _dt.tz_convert(None) if getattr(_dt, "tzinfo", None) else _dt.tz_localize(None)
    except Exception as e:
        raise ValueError(f"Invalid fault_onset_iso {meta['fault_onset_iso']!r}: {e}") from e
    try:
        fault_onset_ts = int(meta["fault_onset_ts"])
    except (TypeError, ValueError) as e:
        raise ValueError(f"Invalid fault_onset_ts {meta['fault_onset_ts']!r}: {e}") from e
    esd_ts = meta.get("esd_ts")
    if esd_ts is not None:
        try:
            esd_ts = int(esd_ts)
        except (TypeError, ValueError) as e:
            raise ValueError(f"Invalid esd_ts {meta.get('esd_ts')!r}: {e}") from e
    return fault_onset_dt, fault_onset_ts, esd_ts


def computeEvaluationMetrics(all_scores, all_states, all_times, all_results,
                             fault_onset_dt, fault_onset_ts, esd_ts=None,
                             esd_iso=None, fault_onset_iso=None,
                             q_alpha=None, q_upper=None):
    """Pure metric computation over one scored sweep.

    Zero detections is a valid outcome (metrics carry ``None`` for undefined
    delays — serialized as ``null`` — while every other metric is preserved),
    as are empty inputs (all counts zero, delays ``None``). No plotting or
    file IO happens here, so this is directly unit-testable.

    Works identically for the autoencoder scan and for baseline scans (whose
    ``all_results`` carry ``{"state": ..., "top_signals": []}``).
    """
    all_scores = list(all_scores)
    all_states = list(all_states)
    all_times = list(all_times)
    all_results = list(all_results)

    first_abnormal_time = None
    first_abnormal_res = None
    for res, t in zip(all_results, all_times):
        if t >= fault_onset_dt and res.get("state") == "abnormal":
            first_abnormal_time = t
            first_abnormal_res = res
            break

    first_abnormal_epoch_ms = (
        epochMsUtc(first_abnormal_time) if first_abnormal_time is not None else None
    )
    detection_delay_hours = (
        (first_abnormal_epoch_ms - fault_onset_ts) / 3600000.0
        if first_abnormal_epoch_ms is not None else None)

    lead_time_hours = (
        (esd_ts - first_abnormal_epoch_ms) / 3600000.0
        if esd_ts is not None and first_abnormal_epoch_ms is not None else None)

    # Attribution check at detection: candidate ranking, not root cause.
    top_signals_at_det = ([s["signal"] for s in first_abnormal_res.get("top_signals", [])
                           if isinstance(s, dict) and "signal" in s]
                          if first_abnormal_res is not None else [])
    flare_in_top3 = any("flare_header_press" in s for s in top_signals_at_det)
    feed_in_top3 = any("feed_pressure" in s for s in top_signals_at_det)

    return {
        "n_windows": len(all_scores),
        "fault_onset_iso": fault_onset_iso,
        "first_abnormal_iso": (first_abnormal_time.isoformat()
                               if first_abnormal_time is not None else None),
        "detection_delay_hours": detection_delay_hours,
        "esd_iso": esd_iso,
        "lead_time_before_esd_hours": lead_time_hours,
        "top_signals_at_detection": top_signals_at_det,
        "flare_header_press_in_top3": flare_in_top3,
        "feed_pressure_in_top3": feed_in_top3,
        "primary_fault_in_top3": flare_in_top3 or feed_in_top3,
        "q_alpha": q_alpha,
        "q_upper": q_upper,
    }


def windowEndTimestamps(timestamps: pd.Series, window_size: int, count: int) -> list:
    """Wall timestamps for the first ``count`` sliding windows (end-row stamps)."""
    return [timestamps.iloc[i + window_size - 1] for i in range(count)]


def resolveFeatureIndex(features: list, signal: str) -> int:
    """Exact or unambiguous leaf-name lookup of a feature column index."""
    if signal in features:
        return features.index(signal)
    leaf = signal.split(".")[-1]
    matches = [i for i, f in enumerate(features) if f.endswith("." + leaf) or f == leaf]
    if len(matches) != 1:
        raise ValueError(f"Baseline signal '{signal}' not resolvable in features {features}")
    return matches[0]


def appendFalseAlarmEvidence(metrics: dict, detector, splits: dict, config: dict,
                             config_path: Path | None) -> tuple:
    """Attach held-out FAR evidence, calibration rates and baselines to ``metrics``.

    THE single source of truth for everything under ``false_alarm_*``,
    ``calibration_rate_*``, ``baselines`` and ``provenance`` in
    evaluation_metrics.json — ``evaluate.py`` (standalone) and
    ``orchestrate.py`` (``flare run``) both call this.

    Returns ``(metrics, baseline_scans)``; ``baseline_scans`` is a list of
    dicts with per-baseline timeline statistics/states, consumed by
    :func:`applyBaselineDelays` for detection delay / lead time.
    """
    window_size = detector.window_size
    alpha = float(config.get("model", {}).get("alpha", 0.05))
    calibration_stride = int(getattr(detector, "calibration_stride", 0) or 0)
    baseline_cfg = config.get("baselines", {}) or {}
    baseline_signal = baseline_cfg.get("signal", DEFAULT_BASELINE_SIGNAL)
    k_grid = baseline_cfg.get("k_grid") or DEFAULT_K_GRID
    features = splits["features"]
    cal_raw = np.asarray(splits["cal_raw"], dtype=np.float32)
    heldout_raw = np.asarray(splits["heldout_raw"], dtype=np.float32)
    timeline_raw = np.asarray(splits["timeline_values"], dtype=np.float32)

    # ------------------------------------------------------------------
    # 1) Autoencoder false alarms on HELD-OUT-NORMAL windows.
    #    These windows are never used for fitting, scaling, calibration or
    #    threshold choice, so this FAR (with its small-sample CI) is the
    #    generalization estimate. Overlapping stride-1 windows are primary;
    #    the non-overlapping stride-window_size pass reports the number of
    #    statistically near-independent windows.
    # ------------------------------------------------------------------
    heldout_states = scoreDetectorWindows(detector, heldout_raw, window_size, stride=1)
    heldout_bundle = falseAlarmSummary(heldout_states)
    heldout_indep_bundle = falseAlarmSummary(
        scoreDetectorWindows(detector, heldout_raw, window_size, stride=window_size))
    ci_lo, ci_hi = clopperPearsonInterval(
        heldout_indep_bundle["n_flagged_abnormal"], heldout_indep_bundle["n_windows"], 0.95)
    heldout_source = splits.get("heldout_source")

    if heldout_bundle["n_windows"] == 0:
        heldout_warning = "held-out split is empty for this configuration"
    elif heldout_source == "independent_capture":
        heldout_warning = None
    else:
        heldout_warning = (
            "held-out rows come from the SAME simulation as train/calibration "
            "(chronological split): they block direct contamination, but the "
            "rows are autocorrelated across the split boundary, so this is "
            "not an independent process run")

    metrics["false_alarm_rate_heldout"] = {
        "source_note": ("held-out normal windows; NOT used for fitting, scaling, "
                        "calibration or threshold choice"),
        "heldout_source": heldout_source,
        "heldout_source_warning": heldout_warning,
        **heldout_bundle,
    }
    metrics["false_alarm_rate_heldout_non_overlapping"] = {
        "source_note": "stride = window_size windows: far fewer, far less autocorrelated",
        "n_independent_windows": heldout_indep_bundle["n_windows"],
        "n_flagged_uncertain_or_abnormal":
            heldout_indep_bundle["n_flagged_total_uncertain_or_abnormal"],
        "n_flagged_abnormal": heldout_indep_bundle["n_flagged_abnormal"],
        "rate_abnormal_only": heldout_indep_bundle["rate_abnormal_only"],
        "clopper_pearson_95_abnormal_only": [float(ci_lo), float(ci_hi)],
        "n_windows_note": ("n_independent_windows is small; the CI upper bound, "
                           "not the point estimate, is the defensible number"),
    }
    metrics["false_alarm_rate_total"] = float(heldout_bundle["rate_total"])
    metrics["false_alarm_rate_abnormal_only"] = float(heldout_bundle["rate_abnormal_only"])
    metrics["false_alarm_rate_note"] = (
        "false_alarm_rate_* are computed on HELD-OUT-NORMAL windows. Earlier "
        "revisions computed them on the calibration split — those (circular) "
        "numbers live under calibration_rate_*.")

    # ------------------------------------------------------------------
    # 2) Calibration-split rates, kept under explicitly circular keys.
    #    The calibration windows themselves define q_alpha, so their
    #    exceedance rate is a property of the quantile construction, NOT an
    #    estimate of generalization. Kept for provenance/comparison only.
    # ------------------------------------------------------------------
    cal_bundle = falseAlarmSummary(
        scoreDetectorWindows(detector, cal_raw, window_size, stride=1))
    n_cal_effective = (len(range(0, cal_bundle["n_windows"], calibration_stride))
                       if calibration_stride and cal_bundle["n_windows"] else 0)
    metrics["calibration_windows"] = cal_bundle["n_windows"]
    metrics["n_calibration_effective_windows"] = n_cal_effective
    metrics["calibration_rate_total"] = float(cal_bundle["rate_total"])
    metrics["calibration_rate_abnormal_only"] = float(cal_bundle["rate_abnormal_only"])
    metrics["calibration_rate_note"] = (
        "computed on the SAME calibration windows that set q_alpha — "
        "circular by construction, NOT an estimate of false-alarm rate on "
        "unseen data. See false_alarm_rate_heldout for the held-out estimate.")
    metrics["never_flag_regime"] = bool(
        detector.q_alpha is not None and not np.isfinite(detector.q_alpha))
    if metrics["never_flag_regime"]:
        need = int(np.ceil((1.0 - alpha) / alpha))
        metrics["never_flag_note"] = (
            f"q_alpha is infinite: with calibration_stride={calibration_stride} "
            f"the calibration split yields only {n_cal_effective} effective "
            f"windows; a finite (1-{alpha:g}) conformal quantile needs >= {need}. "
            "Under split-conformal's never-flag rule the detector cannot emit "
            "UNCERTAIN/ABNORMAL, so FAR/delay metrics degrade to 'never flags' "
            "rather than a calibrated 5% error rate.")

    # ------------------------------------------------------------------
    # 3) Baselines — single-signal thresholds on flare_header_press
    #    (window-mean level and window-mean per-tick slope), k chosen on the
    #    calibration split ONLY (no peeking at held-out or fault data).
    # ------------------------------------------------------------------
    try:
        col = resolveFeatureIndex(features, baseline_signal)
    except ValueError as e:
        metrics["baselines"] = {"error": str(e)}
        return metrics, []

    metrics["baselines"] = {}
    baseline_scans = []
    for statistic in (BASELINE_LEVEL, BASELINE_SLOPE):
        train_stats = windowStatisticSeries(
            np.asarray(splits["train_raw"], dtype=np.float32),
            col, window_size, stride=1, statistic=statistic)
        cal_stats = windowStatisticSeries(cal_raw, col, window_size, stride=1,
                                          statistic=statistic)
        threshold_info = calibrateBaselineThreshold(train_stats, cal_stats, alpha, k_grid)
        timeline_stats = windowStatisticSeries(timeline_raw, col, window_size,
                                               stride=1, statistic=statistic)
        timeline_states = thresholdStates(timeline_stats, threshold_info["threshold"])
        heldout_subset = windowStatisticSeries(heldout_raw, col, window_size,
                                               stride=1, statistic=statistic)
        heldout_indep = windowStatisticSeries(heldout_raw, col, window_size,
                                              stride=window_size, statistic=statistic)
        heldout_summary = falseAlarmSummary(
            thresholdStates(heldout_subset, threshold_info["threshold"]))
        indep_summary = falseAlarmSummary(
            thresholdStates(heldout_indep, threshold_info["threshold"]))
        lo, hi = clopperPearsonInterval(indep_summary["n_flagged_abnormal"],
                                        indep_summary["n_windows"], 0.95)
        stat_name = "window mean" if statistic == BASELINE_LEVEL else "mean per-tick slope"
        label = f"{baseline_signal} ({stat_name})"
        baseline_scans.append({
            "label": label,
            "stats": [float(s) for s in timeline_stats],
            "states": timeline_states,
        })
        metrics["baselines"][label] = {
            "description": (
                f"{stat_name} of {baseline_signal} over the {window_size}-row window; "
                "threshold = train mean + k*std with k chosen so the "
                f"calibration-window FAR <= alpha={alpha} (no peeking at held-out "
                "or fault-day data)"),
            "statistic": statistic,
            "k": threshold_info["k"],
            "threshold": threshold_info["threshold"],
            "threshold_source": threshold_info["mean_std_source"],
            "calibration_flag_rate": threshold_info["calibration_flag_rate"],
            "calibration_n_windows": threshold_info["calibration_n_windows"],
            "calibration_budget_respected": threshold_info["budget_respected"],
            "heldout": heldout_summary,
            "heldout_non_overlapping": indep_summary,
            "clopper_pearson_95_abnormal_only": [float(lo), float(hi)],
            "detection_delay_hours": None,  # filled by applyBaselineDelays
            "lead_time_before_esd_hours": None,
            "first_abnormal_iso": None,
        }
    return metrics, baseline_scans


def applyBaselineDelays(metrics: dict, baseline_scans: list, timestamps: pd.Series,
                        window_size: int, fault_onset_dt, fault_onset_ts,
                        esd_ts=None, meta: dict = None) -> None:
    """Detection delay / lead time for each baseline, in place.

    Baselines flag a window when its statistic exceeds the calibration-
    chosen threshold; delay/lead reuse the identical sweep core
    (computeEvaluationMetrics) as the autoencoder scan.
    """
    for scan in baseline_scans:
        label = scan["label"]
        entry = metrics.get("baselines", {}).get(label)
        if entry is None:
            continue
        times = windowEndTimestamps(timestamps, window_size, len(scan["stats"]))
        delay_metrics = computeEvaluationMetrics(
            scan["stats"], scan["states"], times,
            [{"state": s, "top_signals": []} for s in scan["states"]],
            fault_onset_dt, fault_onset_ts, esd_ts,
            esd_iso=(meta or {}).get("esd_iso"),
            fault_onset_iso=(meta or {}).get("fault_onset_iso"))
        entry["detection_delay_hours"] = delay_metrics["detection_delay_hours"]
        entry["lead_time_before_esd_hours"] = delay_metrics["lead_time_before_esd_hours"]
        entry["first_abnormal_iso"] = delay_metrics["first_abnormal_iso"]


def compareDetectorAndBaselines(metrics: dict) -> None:
    """Plain-language verdict: does any baseline detect as early as the AE?"""
    ae_delay = metrics.get("detection_delay_hours")
    ae_detected = metrics.get("first_abnormal_iso") is not None
    baselines = {k: v for k, v in metrics.get("baselines", {}).items()
                 if isinstance(v, dict) and "detection_delay_hours" in v}
    best_name, best_delay = None, None
    for name, b in baselines.items():
        d = b.get("detection_delay_hours")
        if d is not None and (best_delay is None or d < best_delay):
            best_name, best_delay = name, d
    metrics["detector_vs_baselines"] = {
        "detector_detected": ae_detected,
        "detector_detection_delay_hours": ae_delay,
        "best_baseline": best_name,
        "best_baseline_detection_delay_hours": best_delay,
        "summary": None,
    }
    if not ae_detected:
        summary = "The conformal autoencoder produced NO detection."
        if best_delay is not None:
            summary += (f" The fastest baseline ({best_name}) detected in "
                        f"{best_delay:.1f} h, so the baseline is strictly better "
                        "on this scenario.")
        else:
            summary += " No baseline detected either."
    elif best_delay is not None and best_delay <= ae_delay:
        summary = (f"A baseline detects as early as the autoencoder "
                   f"(baseline: {best_delay:.1f} h vs autoencoder: {ae_delay:.1f} h) — "
                   "the learned detector adds no earliness on this scenario, "
                   "only (hopefully) a lower false-alarm rate.")
    elif best_delay is not None:
        summary = (f"Autoencoder detects {ae_delay - best_delay:.1f} h earlier than "
                   f"the fastest baseline ({best_name}: {best_delay:.1f} h).")
    else:
        summary = "No baseline detected the fault; the autoencoder did."
    metrics["detector_vs_baselines"]["summary"] = summary


def evaluateDetector(detector, splits: dict, config: dict, meta: dict,
                     config_path: Path | None, fault_onset_dt, fault_onset_ts,
                     esd_ts=None) -> dict:
    """Full evaluation of one detector over the archived timeline.

    Returns the metrics dict for evaluation_metrics.json (held-out FAR with
    CI, calibration rates, baselines with delay/lead, provenance).
    """
    timeline_raw = np.asarray(splits["timeline_values"], dtype=np.float32)
    timestamps = splits["timestamps"]
    window_size = detector.window_size
    n_windows = max(0, len(timeline_raw) - window_size + 1)

    all_scores, all_states, all_results = [], [], []
    for i in range(n_windows):
        res = detector.detect(timeline_raw[i:i + window_size])
        all_scores.append(res["score"])
        all_states.append(res["state"])
        all_results.append(res)
    all_times = windowEndTimestamps(timestamps, window_size, len(all_scores))

    metrics = computeEvaluationMetrics(
        all_scores, all_states, all_times, all_results,
        fault_onset_dt, fault_onset_ts, esd_ts,
        esd_iso=meta.get("esd_iso"), fault_onset_iso=meta.get("fault_onset_iso"),
        q_alpha=detector.q_alpha, q_upper=detector.q_upper)

    # Dimensional attribution at detection (ranking, not root cause)
    metrics["primary_dimension"] = None
    metrics["primary_subsystem"] = None
    metrics["dimension_breakdown"] = {}
    if metrics["first_abnormal_iso"]:
        for res, iso in zip(all_results, [t.isoformat() for t in all_times]):
            if iso == metrics["first_abnormal_iso"]:
                metrics["primary_dimension"] = res.get("primary_dimension")
                metrics["primary_subsystem"] = res.get("primary_subsystem")
                metrics["dimension_breakdown"] = res.get("dimension_breakdown", {})
                break

    metrics, baseline_scans = appendFalseAlarmEvidence(
        metrics, detector, splits, config, config_path)

    applyBaselineDelays(metrics, baseline_scans, timestamps, window_size,
                        fault_onset_dt, fault_onset_ts, esd_ts, meta)
    compareDetectorAndBaselines(metrics)

    metrics["provenance"] = collectProvenance(config, config_path)
    metrics["provenance"].update({
        "heldout_source": splits.get("heldout_source"),
        "heldout_archive": splits.get("heldout_archive"),
        "split_rows": splits.get("split_rows"),
        "window_size_rows": int(window_size),
        "stride": int(config.get("dataset", {}).get("stride", 1)),
        "alpha": float(config.get("model", {}).get("alpha", 0.05)),
        "k_multiplier": float(config.get("model", {}).get("k_multiplier", 1.4)),
        "calibration_stride": int(getattr(detector, "calibration_stride", 0) or 0),
        "fault_onset_iso": meta.get("fault_onset_iso"),
        "esd_iso": meta.get("esd_iso"),
    })
    metrics["n_windows"] = len(all_scores)
    return metrics


# ======================================================================================
# Standalone entry point
# ======================================================================================

def runEvaluation(save_plot: bool = True, model_dir: Path = DEFAULT_MODEL_DIR,
                  output_dir: Path = FLARE_DIR, config_path: Path | None = None,
                  archive_path: Path | None = None, meta_path: Path | None = None) -> dict:
    """Evaluate one trained detector and write evaluation_metrics.json (+ plot).

    Default paths address the experiment directory; ``orchestrate.py`` passes
    its run directory's archive/meta so pipeline runs and standalone runs go
    through the identical code path.
    """
    archive_path = Path(archive_path) if archive_path is not None else ARCHIVE
    meta_path = Path(meta_path) if meta_path is not None else META_PATH
    config = loadConfig(Path(config_path)) if config_path is not None else loadConfig(CONFIG_PATH)
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    try:
        fault_onset_dt, fault_onset_ts, esd_ts = parseEvaluationTimestamps(meta)
    except ValueError as e:
        print(f"[evaluate] ERROR: {e}")
        raise SystemExit(f"Invalid experiment metadata: {e}")

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    model_dir = resolveModelDir(model_dir)

    splits = getSplitsAndWindows(archive_path=archive_path, config=config)
    detector = FlareDetector.load(model_dir, config)
    if detector.window_size != getWindowRows(config):
        raise SystemExit(
            f"[evaluate] model window_size={detector.window_size} does not match "
            f"config window size {getWindowRows(config)}; retrain the model.")

    metrics = evaluateDetector(
        detector, splits, config, meta,
        Path(config_path) if config_path else CONFIG_PATH,
        fault_onset_dt, fault_onset_ts, esd_ts)

    printEvaluationSummary(metrics, splits, meta)

    metrics_path = output_dir / "evaluation_metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(f"[evaluate] Metrics saved to {metrics_path}")

    if save_plot:
        renderEvaluationPlot(metrics, splits, detector, meta, output_dir)
    return metrics


def _fmtHour(value) -> str:
    return "None" if value is None else f"{value:.1f} h"


def _fmtPct(value) -> str:
    return "n/a" if value is None else f"{value * 100:.2f}%"


def printEvaluationSummary(metrics: dict, splits: dict, meta: dict) -> None:
    heldout = metrics.get("false_alarm_rate_heldout", {})
    heldout_indep = metrics.get("false_alarm_rate_heldout_non_overlapping", {})
    ci = heldout_indep.get("clopper_pearson_95_abnormal_only", [None, None])
    ci_txt = "n/a" if ci[0] is None else f"[{ci[0] * 100:.2f}%, {ci[1] * 100:.2f}%]"
    print("\n" + "=" * 88)
    print("                          EVALUATION METRICS SUMMARY")
    print("=" * 88)
    sr = splits.get("split_rows") or {}
    print(f"  Held-out source:                  {splits.get('heldout_source')} "
          f"(train {sr.get('train')} / cal {sr.get('calibration')} / "
          f"heldout {sr.get('heldout')} / test {sr.get('test')} rows, "
          f"unused pre-fault: {sr.get('unused_pre_fault_rows')})")
    print("-" * 88)
    print(f"  Held-out windows (stride 1):      {heldout.get('n_windows', 'n/a')}  "
          f"flagged(unc+abn)={heldout.get('n_flagged_total_uncertain_or_abnormal', 'n/a')}  "
          f"flagged(abn)={heldout.get('n_flagged_abnormal', 'n/a')}")
    print(f"  FAR total (uncertain/abnormal):   {_fmtPct(heldout.get('rate_total'))}")
    print(f"  FAR abnormal only:                {_fmtPct(heldout.get('rate_abnormal_only'))}")
    print(f"  Non-overlapping windows:          "
          f"{heldout_indep.get('n_independent_windows', 'n/a')}  "
          f"flagged(abn)={heldout_indep.get('n_flagged_abnormal', 'n/a')}  "
          f"rate={_fmtPct(heldout_indep.get('rate_abnormal_only'))}  "
          f"CP95(abn)={ci_txt}")
    print("-" * 88)
    print(f"  Calibration windows (circular):   {metrics.get('calibration_windows')}  "
          f"effective(stride)={metrics.get('n_calibration_effective_windows')}")
    print(f"  Calibration rate total:           "
          f"{_fmtPct(metrics.get('calibration_rate_total'))}   "
          "(NOT an estimate of generalization)")
    print(f"  Calibration rate abnormal:        "
          f"{_fmtPct(metrics.get('calibration_rate_abnormal_only'))}   "
          "(NOT an estimate of generalization)")
    if metrics.get("never_flag_regime"):
        print("  ! q_alpha = INF — never-flag regime; detector cannot emit any alert.")
    print("-" * 88)
    print(f"  Fault Onset:                      {meta.get('fault_onset_iso')}")
    if metrics.get("first_abnormal_iso"):
        print(f"  First Abnormal Detection:         {metrics['first_abnormal_iso']} "
              f"(delay {metrics['detection_delay_hours']:.1f} sim h)")
    else:
        print("  First Abnormal Detection:         None (detector never flags at/after onset)")
    print(f"  Lead Time Before ESD:             {_fmtHour(metrics.get('lead_time_before_esd_hours'))}")
    print(f"  Top-3 Signals at Detection:       {metrics.get('top_signals_at_detection')}")
    print(f"  Attribution (ranking, not cause): flare={metrics.get('flare_header_press_in_top3')} "
          f"feed={metrics.get('feed_pressure_in_top3')}")
    print("-" * 88)
    print("  BASELINES (threshold = train mean + k*std of the statistic; k chosen on the "
          "calibration split only):")
    for name, b in metrics.get("baselines", {}).items():
        if not isinstance(b, dict) or "detection_delay_hours" not in b:
            continue
        ci_b = b.get("clopper_pearson_95_abnormal_only", [None, None])
        print(f"    {name}")
        print(f"      k={b['k']:.2f}  threshold={b['threshold']:.4f}  "
              f"cal-FAR={_fmtPct(b['calibration_flag_rate'])}  "
              f"held-out FAR(abn)={_fmtPct(b['heldout']['rate_abnormal_only'])} "
              f"(n={b['heldout']['n_windows']}, "
              f"independent n={b['heldout_non_overlapping']['n_windows']}, "
              f"CP95 indep=[{ci_b[0] * 100:.2f}%, {ci_b[1] * 100:.2f}%])")
        print(f"      detection delay: {_fmtHour(b['detection_delay_hours'])}   "
              f"lead time before ESD: {_fmtHour(b['lead_time_before_esd_hours'])}")
    if metrics.get("detector_vs_baselines", {}).get("summary"):
        print("-" * 88)
        print(f"  == {metrics['detector_vs_baselines']['summary']}")
    print("=" * 88 + "\n")


def renderEvaluationPlot(metrics: dict, splits: dict, detector, meta: dict,
                         output_dir: Path) -> None:
    """Reconstruction-score timeline with thresholds, onset, ESD, detections."""
    plot_path = Path(output_dir) / "evaluation_plot.png"
    timeline_raw = np.asarray(splits["timeline_values"], dtype=np.float32)
    timestamps = splits["timestamps"]
    window_size = detector.window_size
    n_windows = max(0, len(timeline_raw) - window_size + 1)
    if n_windows == 0:
        print("[evaluate] No scored windows; skipping plot.")
        return

    all_scores, all_times = [], []
    for i in range(n_windows):
        res = detector.detect(timeline_raw[i:i + window_size])
        all_scores.append(res["score"])
        all_times.append(timestamps.iloc[i + window_size - 1])

    fig, ax = plt.subplots(figsize=(14, 6), dpi=150)
    start_t = timestamps.iloc[0]
    elapsed_hours = [(t - start_t).total_seconds() / 3600.0 for t in all_times]
    ax.plot(elapsed_hours, all_scores, label="Reconstruction Score (MSE)",
            color="#1f77b4", linewidth=1.5)

    max_y = min(150.0, max(all_scores) * 1.1)
    ax.set_ylim(-0.1, max_y)
    q_alpha, q_upper = detector.q_alpha, detector.q_upper
    if np.isfinite(q_alpha):
        ax.axhline(q_alpha, color="#ff7f0e", linestyle="--", linewidth=1.2,
                   label=f"q_alpha ({q_alpha:.3f})")
        ax.axhspan(-0.1, q_alpha, color="green", alpha=0.08, label="Normal Zone")
    if np.isfinite(q_upper):
        ax.axhline(q_upper, color="#d62728", linestyle="--", linewidth=1.2,
                   label=(f"q_upper = {detector.k_multiplier:g} * q_alpha — heuristic "
                          f"margin, no conformal guarantee ({q_upper:.3f})"))
    if np.isfinite(q_alpha) and np.isfinite(q_upper):
        ax.axhspan(q_alpha, q_upper, color="orange", alpha=0.10, label="Uncertain Zone")
        if max_y > q_upper:
            ax.axhspan(q_upper, max_y, color="red", alpha=0.08, label="Abnormal Zone")

    fault_onset_dt, _onset_ts, _esd_ts = parseEvaluationTimestamps(meta)
    fault_elapsed = (fault_onset_dt - start_t).total_seconds() / 3600.0
    ax.axvline(fault_elapsed, color="purple", linestyle="-.", linewidth=1.5,
               label=f"Fault Onset (t={fault_elapsed:.0f}h)")

    if metrics.get("esd_iso"):
        esd_dt = pd.to_datetime(metrics["esd_iso"])
        esd_dt = esd_dt.tz_convert(None) if getattr(esd_dt, "tzinfo", None) else esd_dt
        esd_elapsed = (esd_dt - start_t).total_seconds() / 3600.0
        ax.axvline(esd_elapsed, color="black", linestyle=":", linewidth=1.8,
                   label=f"ESD Trip (t={esd_elapsed:.0f}h)")

    first_iso = metrics.get("first_abnormal_iso")
    if first_iso:
        first_time = pd.to_datetime(first_iso)
        first_time = (first_time.tz_convert(None) if getattr(first_time, "tzinfo", None)
                      else first_time)
        det_elapsed = (first_time - start_t).total_seconds() / 3600.0
        det_score = None
        for t, s in zip(all_times, all_scores):
            if t == first_time:
                det_score = s
                break
        ax.scatter([det_elapsed], [det_score if det_score is not None else max_y * 0.5],
                   color="red", marker="*", s=220, zorder=6,
                   label=f"Detector Detection (t={det_elapsed:.0f}h)")

    baseline_colors = ("#2ca02c", "#8c564b")
    baseline_labels = sorted(k for k, v in metrics.get("baselines", {}).items()
                             if isinstance(v, dict) and "detection_delay_hours" in v)
    for name, color in zip(baseline_labels, baseline_colors):
        iso = metrics["baselines"][name].get("first_abnormal_iso")
        if not iso:
            continue
        btime = pd.to_datetime(iso)
        btime = btime.tz_convert(None) if getattr(btime, "tzinfo", None) else btime
        belapsed = (btime - start_t).total_seconds() / 3600.0
        ax.axvline(belapsed, color=color, linestyle="--", linewidth=1.2, alpha=0.8,
                   label=f"Baseline first detection ({name}, t={belapsed:.0f}h)")

    ax.set_title("Flare Blocked Anomaly Detection — Autoencoder Reconstruction Score vs Time\n"
                 "(FAR from held-out-normal windows with Clopper–Pearson CI; baselines "
                 "calibrated on the calibration split only)",
                 fontsize=13, fontweight="bold", pad=12)
    ax.set_xlabel("Simulated Plant Operation (Hours)", fontsize=11)
    ax.set_ylabel("Reconstruction Error Score", fontsize=11)
    ax.grid(True, linestyle=":", alpha=0.6)
    ax.legend(loc="upper left", fontsize=8, framealpha=0.9)
    fig.tight_layout()
    fig.savefig(plot_path)
    plt.close(fig)
    print(f"[evaluate] Plot saved to {plot_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-plot", action="store_true", help="Skip saving evaluation plot")
    parser.add_argument("--model-dir", default=str(DEFAULT_MODEL_DIR),
                        help="Directory holding model.pt + model_meta.json "
                             "(train_detector.py --save-dir <dir> writes <dir>/model/)")
    parser.add_argument("--output-dir", default=str(FLARE_DIR),
                        help="Directory for evaluation_metrics.json and evaluation_plot.png")
    parser.add_argument("--config", default=None,
                        help="Config file (YAML/JSON); default: experiment dir config.yaml")
    args = parser.parse_args()

    runEvaluation(save_plot=not args.no_plot, model_dir=Path(args.model_dir),
                  output_dir=Path(args.output_dir),
                  config_path=Path(args.config) if args.config else None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
