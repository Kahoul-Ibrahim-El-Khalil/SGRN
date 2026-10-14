# SGRN Autonomous Supervision — Flare Blocked Anomaly Detection Report

**Generated:** 2026-10-14 09:01:29 UTC  
**Target Output Directory:** `/home/odahim/Studies/Software/SGRN/experiments/flare`

---

## Executive Summary

The physics-informed split-conformal manifold detector was evaluated across the full 31-day (742-hour) natural gas molecular sieve plant simulation.

| Metric | Result | Target Benchmark | Status |
|:---|:---:|:---:|:---:|
| **False Alarm Rate (Abnormal Only)** | **0.00%** | < 0.01% | **PASS** |
| **Detection Delay** | **1.0 hours** | <= 2.0 hours | **PASS** |
| **Lead Time Before Emergency Shutdown (ESD)** | **18.0 hours** | >= 12.0 hours | **PASS** |
| **Primary Drifting Feature Dimension** | **`SafetySystems.flare_header_press`** | `SafetySystems.flare_header_press` | **PASS** |
| **Primary Subsystem / Device** | **`SafetySystems`** | `SafetySystems` | **PASS** |
| **Flare Header in Top-3 Deviations** | **True** | True | **PASS** |
| **Feed Pressure in Top-3 Deviations** | **True** | True | **PASS** |

---

## Split-Conformal Calibration Thresholds

- **Calibration Sample Size (n_cal)**: 70 12-hour temporal windows
- **Conformal Threshold (q_alpha, 1 - alpha = 0.95)**: `0.7416`
- **Upper Safety Bound (q_upper, k = 1.4)**: `1.0383`

---

## Top Feature Dimension Deviations at Detection (t = 2000-01-31T04:00:00)

| Feature Dimension | Residual MSE | Contribution Percentage | Subsystem / Device |
|:---|:---:|:---:|:---:|
| `SafetySystems.flare_header_press` | 22.1031 | 58.64% | SafetySystems |
| `InletSeparation.feed_pressure` | 2.5999 | 6.90% | InletSeparation |
| `InletSeparation.scrubber_press` | 2.4470 | 6.49% | InletSeparation |
| `AdsorberTowers.system_inlet_press` | 1.6161 | 4.29% | AdsorberTowers |
| `InletSeparation.feed_flow.rate` | 1.1921 | 3.16% | InletSeparation |


Top-3 deviating process signals:
1. `SafetySystems.flare_header_press`
2. `InletSeparation.feed_pressure`
3. `InletSeparation.scrubber_press`

---

## Artifact Manifest


- Telemetry Archive: `flare_history.jsonl.zst`
- Extracted Feature Dataset: `dataset.csv`
- Model Checkpoint: `model.pt`
- Calibration Metadata: `model_meta.json`
- Quantitative Metrics: `evaluation_metrics.json`
- Multi-Panel Visualization: `evaluation_plot.png`
