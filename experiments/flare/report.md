# SGRN Autonomous Supervision — Flare Blocked Anomaly Detection Report

**Generated:** 2026-10-19 18:44:58 UTC  
**Target Output Directory:** `/home/odahim/Studies/Software/SGRN/experiments/flare`

---

## Executive Summary

The split-conformal manifold detector was evaluated across the full archived plant simulation. The false-alarm rate is measured on **held-out-normal windows** (never on the calibration split used to set q_alpha — that measurement is circular by construction and lives under `calibration_rate_*` in `evaluation_metrics.json`). Detection delay is the time from fault onset to the first ABNORMAL window; lead time is the remaining time before the simulated ESD trip. Single-signal baselines (threshold = train mean + k·std on the raw flare-header pressure level and slope, with k chosen on the calibration split only) are reported alongside for context.

| Metric | Result | Target Benchmark | Status |
|:---|:---:|:---:|:---:|
| **Held-Out-Normal FAR (Abnormal Only)** | **0.00% (never-flag regime: q_alpha infinite, see limitations)** | ≤ 5.0% conformal budget | **PASS** |
| **Held-Out-Normal FAR (Uncertain or Abnormal)** | **0.00%** (autoencoder) | ≤ alpha + margin | informational |
| Independent windows (stride = window size) | 5 | flagged: 0 | FAR 95% CI ≈ [47.82%, 100.00%] |
| **Detection Delay** | **n/a (no detection)** | ≤ 2.0 hours | **FAIL** |
| **Lead Time Before Emergency Shutdown (ESD)** | **n/a (no detection)** | ≥ 12.0 hours | **FAIL** |
| **Primary Drifting Feature Dimension** | **`None`** | `SafetySystems.flare_header_press` | **FAIL** |
| **Primary Subsystem / Device** | **`None`** | `SafetySystems` | **FAIL** |
| **Flare Header in Top-3 Deviations** | **False** | True | **FAIL** |
| **Feed Pressure in Top-3 Deviations** | **False** | True | **FAIL** |

**Detector vs baselines:** The conformal autoencoder produced NO detection. The fastest baseline (SafetySystems.flare_header_press (window mean)) detected in 0.0 h, so the baseline is strictly better on this scenario.

## Baselines (same data, calibrated on the calibration split only)

| Baseline | Held-out FAR (abnormal only) | Detection delay | Lead time before ESD |
|:---|:---:|:---:|:---:|
| SafetySystems.flare_header_press (window mean) (= train μ + 2.50σ) | 0.00% (0/56) | 0.0 hours | 19.0 hours |
| SafetySystems.flare_header_press (mean per-tick slope) (= train μ + 1.75σ) | 7.14% (4/56) | 0.0 hours | 19.0 hours |


---

## Split-Conformal Calibration Thresholds

- **Calibration Sample Size (n_cal, stride-1 windows)**: 55 raw / 5 effective (thinned at stride n/a)
- **Conformal Threshold (q_alpha)**: `inf`
- **Upper Safety Bound (q_upper, heuristic multiplier, NOT a calibrated level)**: `inf`
- Calibration-split exceedance rates (`calibration_rate_*` in JSON) are **circular**: the calibration windows define q_alpha. They are recorded for provenance only.

---

## Top Feature Dimension Deviations at Detection (t = no detection)

| Feature Dimension | Residual MSE | Contribution Percentage | Subsystem / Device |
|:---|:---:|:---:|:---:|


---

## Limitations

- A single fault scenario (`FLARE_BLOCKED`, fault_code 9) on our own simulator (`simulation.as`) — the only available data source. No real plant data, no cross-scenario validation.
- The held-out normal data comes from a chronological split of the same pre-fault run (row-to-row autocorrelation crosses the split boundary); the simulator's noise LCG is not seed-controlled, so a re-captured "normal" run replays the same trajectory rather than providing an independent realization.
- The fault is a LINEAR RAMP on `flare_header_press` (plus hydraulic backpressure coupling) — the detector attributes the anomaly to that same ramp; attribution is correlational ranking, not causal proof.
- Simulated telemetry is persisted on change only, so rows are irregularly spaced; sliding "12-hour" windows cover variable physical durations.
- Sliding windows with stride 1 are strongly autocorrelated: the vanilla conformal coverage statement does not formally hold; q_upper = k·q_alpha is a heuristic alerting margin, not a second conformal quantile.
- Small effective sample everywhere (single 31-day run); all rates carry tiny-sample CIs, and a different capture could legitimately produce different numbers.

## Artifact Manifest

- Telemetry Archive: `flare_history.jsonl.zst`
- Extracted Feature Dataset: `dataset.csv`
- Model Checkpoint: `model.pt`
- Calibration Metadata: `model_meta.json`
- Quantitative Metrics: `evaluation_metrics.json`
- Multi-Panel Visualization: `evaluation_plot.png`
