# Minimal Partial-Loop Prototype: Flare Blocked Anomaly Detection

## Start here

All commands assume your current working directory is the **SGRN repository root**.

| Goal | Command |
|:---|:---|
| Check Python deps, config, and artifacts | `python3 experiments/flare check` |
| **Full pipeline** (dataset → train → evaluate → report) | `python3 experiments/flare run` |
| Metrics + plot only (needs `model.pt`) | `python3 experiments/flare evaluate` |
| Live replay demo (no API key) | `python3 experiments/flare demo` |
| Regenerate telemetry WAL from simulation (slow; needs built binaries) | `python3 experiments/flare capture` |

Install Python deps once (in your env): `pip install -r experiments/flare/requirements.txt`

**Config:** If both `config.yaml` and `config.json` are present, **YAML wins**. Keep `dataset.window_size.size` in sync (default **12** simulated hours).

**Advanced:** `python3 experiments/flare run --help` forwards to `orchestrate.py` (e.g. `--output-dir scratch/run`, `--capture`, `--epochs 20`).

---

This document provides a comprehensive specification and execution guide for the **Flare Blocked (`fault_code 9`)** experiment within the SGRN framework.

The experiment validates a fundamental slice of autonomous plant supervision:
$$\text{Physics Simulation (s7shell)} \longrightarrow \text{SGRN Gateway} \longrightarrow \text{Calibrated Autoencoder Detector} \longrightarrow \text{Read-Only LangChain Agent}$$

---

## 1. System & Architecture Overview

The entire system is strictly **schema-driven**. Everything—from twin memory offsets, REST APIs, and WebSocket streams, to OPC-UA node hierarchies and write-ahead log (WAL) dictionaries—is derived directly from the industrial SCL schema. No custom byte offsets, communication drivers, or protocol decoders are written in user space; all industrial plumbing is abstracted into the core SGRN runtime.

```mermaid
flowchart TD
    subgraph PROCESS["1. Physical Process (s7shell Soft-PLC)"]
        SCL["schema.scl<br/>(Single Source of Truth)"] --> SIM["simulation.as<br/>(3-Tower Molecular Sieve Physics)"]
        SIM -->|"Day 31, Hour 3: Inject Fault 9"| FAULT["FLARE_BLOCKED<br/>flare_press ↑, feed_press ↑"]
    end

    subgraph INGESTION["2. Gateway & Ingestion"]
        SIM -->|"WebSocket Deltas"| GW["SGRN Gateway<br/>(Northbound HTTP/WS)"]
    GW -->|"Streaming Zstd WAL"| WAL[("flare_history.jsonl.zst<br/>31 Days / ~344 persist-on-change rows")]
        GW -.->|"OPC-UA Server (Port 4840)<br/>Objects/SGRN/*"| OPC["OPC-UA Clients / Forcing"]
    end

    subgraph DETECTION["3. ML & Uncertainty Quantification"]
        WAL -->|"dataset.py"| DS["dataset.csv<br/>(20 Clean Process Signals)"]
        DS -->|"Train Split (60%)"| TR["Autoencoder Training<br/>(240 → 64 → 16 → 64 → 240)"]
        DS -->|"Cal Split (20%)<br/>sets q_alpha only"| CAL["Split-Conformal Calibration<br/>(q_upper = k·q_alpha is a heuristic margin)"]
        DS -->|"Held-Out Normal (20%)<br/>FAR evaluation only"| HO["Held-Out FAR with<br/>Clopper–Pearson CI"]
        TR & CAL --> MODEL[("model.pt & model_meta.json")]
    end

    subgraph TWIN_AGENT["4. Live Replay & Advisory Agent"]
        WAL -->|"Fast/Real-Time Playback"| REP["sgrn_replay (Port 18080)"]
        REP -->|"Live Telemetry"| TWINGW["Prediction Twin Gateway (Port 18082)"]
        TWINGW -->|"Sliding Window (12h)"| DET["FlareDetector Inference"]
        MODEL -.-> DET
        DET -->|"3-State Uncertainty Gate<br/>NORMAL | UNCERTAIN | ABNORMAL"| TOOLS["LangChain Structured Tools<br/>get_latest_detector_state<br/>get_signal_trend<br/>get_replay_status"]
        TOOLS --> AGENT["LangChain Advisory Agent<br/>(Strictly Read-Only Console Output)"]
    end
```

---

## 2. The Process: Molecular Sieve Gas Dehydration

The simulated plant models a **natural gas molecular-sieve dehydration unit** tasked with removing water vapor from a high-pressure feed gas stream down to a strict dew-point specification ($\le -100^\circ\text{C}$):

1. **Inlet Separation (`InletSeparation`)**:
   - High-pressure raw feed enters at nominal $68.0\text{ bar}$, $32.0^\circ\text{C}$, and $850\text{ e}^3\text{m}^3/\text{h}$.
   - Free liquids and water droplets are knocked out across the inlet scrubber and filter coalescer.
2. **Adsorption Cycle (`AdsorberTowers`)**:
   - Three vertical towers packed with synthetic zeolites run a staggered cyclic sequence:
     - **Tower 1**: On-stream Adsorption (removing moisture).
     - **Tower 2**: Regeneration (thermal desorption via heated dry gas).
     - **Tower 3**: Cooling & Standby (cooling down to bed adsorption temperature).
3. **Regeneration Gas Circuit (`RegenSystem`)**:
   - A closed heating/cooling loop: a fuel-gas-fired heater heats regen gas to $290^\circ\text{C}$ during heating, a fin-fan cooler drops it to $<50^\circ\text{C}$ during cooling, and a blower circulates gas.
4. **Sales Gas Quality (`OutletQuality`)**:
   - Dried product gas passes through dust filters to pipeline transmission with online moisture analysis.
5. **Safety Systems (`SafetySystems`)**:
   - Overpressure relief headers routed to the plant flare system.
   - Emergency Shutdown (ESD) logic monitoring pressure, temperature, and dew-point excursions.

### The Fault Scenario: `FLARE_BLOCKED` (`fault_code 9`)
- **Onset**: Day 31 at Hour 3 (tick 723).
- **Physical Mechanism**: A downstream block valve on the main flare relief header is inadvertently closed or frozen. 
- **Signal Dynamics**:
  1. Header pressure (`SafetySystems.flare_header_press`) ramps up linearly from nominal $1.05\text{ bar}$ at $+0.15\text{ bar/tick}$.
  2. Because the relief path is closed, process relief fails, causing feed pressure (`InletSeparation.feed_pressure`) to ramp up at $+1.15\text{ bar/tick}$.
  3. Scrubber pressure and system inlet/outlet pressures track upward with first-order hydraulic lag.
  4. At $t = 723\text{ hours}$ (Day 31, Hour 3 of a 744-hour, 31-day run), the fault is injected; by the ESD tick (Day 31, Hour 22) feed pressure breaches $90.0\text{ bar}$, triggering an automated **Emergency Shutdown (ESD)** trip. Note the WAL persists only *changes*, so the archive holds ~344 sparse rows (2–4 h gaps), not one row per hour.

---

## 3. The Schema (`schema.scl`) as the Single Source of Truth

The entire plant memory map is defined in `schema.scl` across 7 DataBlocks:

| DB | Name | Size (Bytes) | Role & Primary Telemetry |
|:---|:---|:---|:---|
| **DB1** | `InletSeparation` | 94 | Feed gas pressure, temperature, flow rate, scrubber level, coalescer $\Delta P$. |
| **DB2** | `AdsorberTowers` | 130 | 3-tower bed states, differential pressures, moisture loading, total flow. |
| **DB3** | `RegenSystem` | 142 | Regen heater duty/temperature, cooler temperatures, blower flow, knockout level. |
| **DB4** | `SwitchingValves` | 266 | 16 discrete switching and bypass valve actuators and limit switches. |
| **DB5** | `OutletQuality` | 74 | Sales gas moisture dew point, analyzer calibration status, dust filter $\Delta P$. |
| **DB6** | `SafetySystems` | 196 | Flare header pressure, vent valve position, fire/gas detectors, ESD trip signals. |
| **DB7** | `Utilities` | 36 | Instrument air pressure, fuel gas pressure, regen compressor status. |

### How SGRN Derives Interfaces from Schema
- **REST Endpoints**: `/data/InletSeparation`, `/data/SafetySystems`, `/registry` are automatically mounted.
- **WebSocket Topics**: Every leaf becomes a dot-separated identifier (e.g., `SafetySystems.flare_header_press`).
- **OPC-UA Address Space**: DataBlocks map to OPC-UA Object nodes under `Objects/SGRN/`, and fields become Variable nodes (e.g., `ns=2;s=Objects/SGRN/SafetySystems/flare_header_press`).
- **Binary/JSONL WAL**: SGRN assigns discrete integer `LeafId`s to each field for high-throughput zero-copy compression.

---

## 4. Configuration Declarations

### `config.json` (Experiment & Detector Configuration)
Governs scenario timings, split ratios, conformal bounds, and feature selections:

```json
{
  "scenario": {
    "normal_days": 30,
    "fault_day": 31,
    "fault_code": 9,
    "fault_injection_tick": 723,
    "flare_ramp_rate": 0.15,
    "feed_ramp_rate": 1.15,
    "tick_sleep_ms": 10
  },
  "dataset": {
    "train_ratio": 0.60,
    "calibration_ratio": 0.20,
    "heldout_ratio": 0.20,
    "window_size": 12,
    "stride": 1,
    "calibration_stride": 12,
    "random_seed": 42
  },
  "conformal": {
    "alpha": 0.05,
    "uncertain_upper_multiplier": 1.4
  },
  "baselines": {
    "signal": "SafetySystems.flare_header_press",
    "window": 12,
    "k_grid": [1.5, 1.75, 2.0, 2.5, 3.0]
  },
  "features": [
    "InletSeparation.feed_pressure",
    "InletSeparation.scrubber_press",
    "InletSeparation.feed_flow.rate",
    "InletSeparation.feed_temp",
    "InletSeparation.coalescer_dp",
    "AdsorberTowers.system_inlet_press",
    "AdsorberTowers.system_outlet_press",
    "AdsorberTowers.total_gas_flow.rate",
    "RegenSystem.heater_outlet_temp",
    "RegenSystem.cooler_inlet_temp",
    "RegenSystem.cooler_outlet_temp",
    "RegenSystem.blower_flow.rate",
    "OutletQuality.dew_point",
    "OutletQuality.sales_gas_press",
    "OutletQuality.sales_gas_flow.rate",
    "OutletQuality.sales_gas_temp",
    "SafetySystems.flare_header_press",
    "Utilities.fuel_gas_press",
    "Utilities.inst_air_press",
    "Utilities.regen_comp_disch_press"
  ]
}
```

### `gateway.json` (Ingestion & Persistence Configuration)
Controls the Gateway's listeners, rate limits, and historian writer:
- `"northbound.http"` & `"websocket"`: Bind to `127.0.0.1:18080` (or `18082` for twin).
- `"northbound.opcua"`: Binds to `127.0.0.1:4840` for external SCADA integration and signal forcing.
- `"persistence.format"`: `"jsonl"` with Zstandard compression.
- `"persistence.mode"`: `"changes_with_timestamp"` (WAL delta streams).
- `"persistence.atomic_window_ms"`: `1` (clean per-tick delta preservation).

---

## 5. Model Formulation: Physical Manifold Learning & Prediction

### A. The Manifold Hypothesis for Cyber-Physical Plants
In an industrial process, plant state variables (pressures $P$, temperatures $T$, mass flow rates $\dot{m}$, gas densities $\rho$, and valve positions) are **not mutually independent random variables**. They are tightly coupled by governing physical conservation laws and thermodynamic equations of state:

1. **Mass & Mole Conservation**:
   $$\sum \dot{m}_{\text{in}} = \sum \dot{m}_{\text{out}} + \frac{d}{dt}\int_{V} \rho \, dV$$
2. **Real Gas Equation of State**:
   $$P \cdot V = Z(P, T) \cdot n R T$$
3. **Fluid Hydraulics & Momentum Balance (Ergun/Darcy Pressure Drops)**:
   $$\Delta P_{\text{bed}} = 150 \frac{\mu (1-\epsilon)^2}{d_p^2 \epsilon^3} v_s L + 1.75 \frac{\rho (1-\epsilon)}{d_p \epsilon^3} v_s^2 L$$
4. **Heat Transfer Thermodynamics (Regeneration Closed Loop)**:
   $$\dot{Q} = U A \Delta T_{\text{lm}} = \dot{m} C_p (T_{\text{out}} - T_{\text{in}})$$
5. **Adsorption Isotherm Equilibrium (Desiccant Loading)**:
   $$q^*(P_{\text{H}_2\text{O}}, T) = \frac{q_m K P_{\text{H}_2\text{O}}}{1 + K P_{\text{H}_2\text{O}}}$$

Because of these coupled differential-algebraic constraints, the healthy operational trajectories of the plant do not span the entire ambient measurement space:
$$\mathbf{X} \in \mathbb{R}^{240} \quad (20 \text{ continuous process signals} \times 12 \text{ hours history})$$

Instead, the true physical system evolves along a **low-dimensional smooth sub-manifold $\mathcal{M} \subset \mathbb{R}^{240}$** parameterized by only a few intrinsic degrees of freedom (overall feed throughput, ambient weather temperature, heater firing duty, and 3-tower cycle sequencer phase $\theta \in [0, 2\pi)$).

---

### B. Based On What? (The Input Feature Space)
The model consumes a sliding window across **20 continuous process signals** over a 12-hour history horizon ($W = 12$ ticks, where 1 tick = 1 simulated hour):

$$\mathbf{X} \in \mathbb{R}^{12 \times 20} \quad \xrightarrow{\text{flatten}} \quad \mathbf{x} \in \mathbb{R}^{240}$$

Every input dimension is standardized using the mean ($\mu$) and standard deviation ($\sigma$) computed strictly from the nominal pre-fault training set:
$$\widetilde{x}_{t,j} = \frac{x_{t,j} - \mu_j}{\sigma_j}$$

The 20 signals are grouped by physical subsystem:
- **Inlet Separation**: `feed_pressure`, `scrubber_press`, `feed_flow.rate`, `feed_temp`, `coalescer_dp`
- **Adsorber Towers**: `system_inlet_press`, `system_outlet_press`, `total_gas_flow.rate`
- **Regeneration Loop**: `heater_outlet_temp`, `cooler_inlet_temp`, `cooler_outlet_temp`, `blower_flow.rate`
- **Outlet Quality**: `dew_point`, `sales_gas_press`, `sales_gas_flow.rate`, `sales_gas_temp`
- **Safety Systems**: `flare_header_press`
- **Utilities**: `fuel_gas_press`, `inst_air_press`, `regen_comp_disch_press`

> **Strict Boundary Invariant**: All alarm bits, ESD trip flags, fault codes, model predictions, and timestamps are **strictly excluded**. The model receives only continuous physical process measurements.

---

### B.1. Why Use a Sliding Window? (Dynamical Trajectories & Conformal Exchangeability)

Why does the detector ingest a 12-hour sliding window $\mathbf{X}_w = [\mathbf{x}_{t-11}, \dots, \mathbf{x}_t] \in \mathbb{R}^{12 \times 20}$ rather than isolated instantaneous state vectors $\mathbf{x}_t \in \mathbb{R}^{20}$?

1. **Phase-Space Trajectory Reconstruction (Takens' Delay Embedding)**:
   An instantaneous snapshot captures only static pressures and temperatures. It contains no derivative, rate-of-change, or hysteresis information. Stacking $W = 12$ delayed states reconstructs an unobserved dynamical state space diffeomorphic to the underlying physical manifold. The autoencoder models the **dynamic process trajectory** rather than a memoryless point.
2. **Spanning Complete Tower Cycle Dynamics**:
   The molecular sieve dehydration towers cycle through distinct phases (adsorption, thermal regeneration at $290^\circ\text{C}$, cooling to $<50^\circ\text{C}$). One full bed cycle takes 12 hours. A 12-hour window guarantees that every observation contains a full periodic cycle, converting periodic non-stationarity into a **cyclically stationary limit cycle in $\mathbb{R}^{240}$**.
3. **Overlapping Windows Do NOT Restore Conformal Guarantees**:
    Conformal prediction bounds a $1 - \alpha$ false-alarm rate only under **exchangeable** non-conformity scores. Sliding windows with stride 1 share $W-1$ timesteps, so consecutive window scores are strongly autocorrelated and the exchangeability assumption is violated. For calibration we therefore thin to non-overlapping blocks (`dataset.calibration_stride = window size`), which *approximates* block exchangeability — the coverage level is approximate, not a formal finite-sample guarantee, because residual autocorrelation across adjacent blocks and process drift remain. No claim of "quasi-exchangeability" is made for stride-1 windows, and no "0.00% false alarms" figure is a guarantee: false-alarm rates are measured, with raw counts and a Clopper–Pearson confidence interval, on **held-out-normal windows** whose sample size is small enough that the CI, not the point estimate, is the defensible number. (Earlier drafts called the calibration split "held-out"; that was wrong — see the `calibration_rate_*` vs `false_alarm_rate_heldout` split in `evaluation_metrics.json`.)
4. **Transient Glitch Rejection vs. Monotonic Fault Drift**:
   An instrument glitch or valve travel flutter is transient and affects only a single time tick. A true cyber-physical failure (such as `FLARE_BLOCKED`, which ramps flare header pressure at $+0.15\text{ bar/tick}$) produces a coherent, sustained drift across the entire 12-hour trajectory, eliminating spurious trip alerts.

---

### C. What Are We Predicting?

The PyTorch autoencoder learns an empirical coordinate chart and non-linear projection operator onto the physical manifold $\mathcal{M}$:

1. **Non-Linear Manifold Projection ($\widehat{\mathbf{x}} = \operatorname{Proj}_{\mathcal{M}}(\widetilde{\mathbf{x}})$)**:
   The bottleneck neural network decomposes into an encoder $\mathcal{E}: \mathbb{R}^{240} \to \mathbb{R}^{16}$ and a decoder $\mathcal{D}: \mathbb{R}^{16} \to \mathbb{R}^{240}$:
   $$\mathbf{z} = \mathcal{E}(\widetilde{\mathbf{x}}), \quad \widehat{\mathbf{x}} = \mathcal{D}(\mathbf{z}) = \mathcal{D}(\mathcal{E}(\widetilde{\mathbf{x}}))$$
   $\widehat{\mathbf{x}}$ represents the **closest physically consistent plant state lying on $\mathcal{M}$** corresponding to the input trajectory $\widetilde{\mathbf{x}}$.

2. **Orthogonal Manifold Distance as Anomaly Score ($e(\mathbf{x})$)**:
   The reconstruction error measures the squared Euclidean distance from the observed plant state to the physical manifold:
   $$e(\mathbf{x}) = \operatorname{dist}^2(\widetilde{\mathbf{x}}, \mathcal{M}) = \frac{1}{240} \|\widetilde{\mathbf{x}} - \operatorname{Proj}_{\mathcal{M}}(\widetilde{\mathbf{x}})\|_2^2 = \frac{1}{240} \sum_{i=1}^{240} (\widetilde{x}_i - \widehat{x}_i)^2$$
   - **Nominal Plant State**: $\widetilde{\mathbf{x}} \in \mathcal{M} \implies e(\mathbf{x}) \approx 0.15 \text{ to } 0.55$ in earlier runs (attributable to measurement noise and slight turbulence; exact ranges are configuration-dependent, see `model_meta.json` / `evaluation_metrics.json`).
   - **Fault State (`FLARE_BLOCKED`)**: An obstruction prevents relief venting while feed continues to supply mass $\implies$ the physical relationship between header pressure, feed pressure, and valve positions is violated $\implies \widetilde{\mathbf{x}} \notin \mathcal{M} \implies e(\mathbf{x})$ sharply exceeds nominal ranges (orders of magnitude above typical normal-window scores in earlier runs).

3. **Conformal Tolerance Tube & 3-State Uncertainty Classification**:
   Real physical processes possess non-zero turbulence and sensor noise. Therefore, the nominal manifold is surrounded by an empirical tolerance tube $\mathcal{T}_{\alpha}$ whose centre threshold $q_{\alpha}$ is the split-conformal $(1-\alpha)$ quantile of non-conformity scores on a **dedicated calibration split** (never on training or held-out data), at significance level $\alpha = 0.05$:
   $$\mathcal{T}_{\alpha} = \left\{ \mathbf{x} \in \mathbb{R}^{240} \;\middle|\; \operatorname{dist}^2(\widetilde{\mathbf{x}}, \mathcal{M}) \le q_{\text{upper}} \right\}$$
   Two honest caveats about the thresholds actually computed in this experiment:
   - **`q_alpha` is finite-sample random, not a fixed constant.** Its value depends on the calibration split and is recomputed every run; do not treat previously published numbers as properties of the model. (In the current run $q_{\alpha}$ is not even finite — see below.)
   - **`q_upper = k · q_alpha` (k = 1.4) is a heuristic margin, not a conformal quantity.** Split-conformal theory provides no coverage guarantee for this multiplied threshold; it exists only to create an intermediate `UNCERTAIN` band for the agent's abstention logic.
   With `dataset.calibration_stride = 12` (non-overlapping calibration blocks, chosen to reduce autocorrelation), the bundled one-capture dataset yields too few calibration blocks for a finite 95% conformal quantile (which needs ≥ 19 blocks). Under split-conformal's conservative finite-sample rule the quantile is therefore **infinite**, and the detector sits in a **never-flag regime**: it cannot emit `UNCERTAIN` or `ABNORMAL` at all. This is the mathematically conservative outcome, and we report it as such rather than "fixing" it by loosening the rule — see Section 9.

   - **`NORMAL`** ($e < q_{\alpha}$): The plant trajectory lies inside the nominal physical tube $\mathcal{T}_{\alpha}$.
   - **`UNCERTAIN`** ($q_{\alpha} \le e < q_{\text{upper}}$): Near the boundary of the tube; the LangChain agent abstains from hasty alarms during heavy tower switching swings.
   - **`ABNORMAL`** ($e \ge q_{\text{upper}}$): Confident excursion outside the physical manifold; physical laws or operational bounds have been breached.

4. **Directional Error Decomposition for Root-Cause Attribution**:
   The residual vector $\mathbf{r} = \widetilde{\mathbf{x}} - \widehat{\mathbf{x}}$ defines the normal vector pointing from the manifold $\mathcal{M}$ to the anomalous point $\widetilde{\mathbf{x}}$. Computing the per-signal projection error over the 12-hour window:
   $$e_j = \frac{1}{12} \sum_{t=1}^{12} (\widetilde{x}_{t,j} - \widehat{x}_{t,j})^2, \quad j \in \{1, \dots, 20\}$$
   Directly isolates which physical dimensions contributed the greatest displacement from $\mathcal{M}$. **Caveat:** this attribution is only meaningful when the detector actually flags a window. In the current never-flag regime (Section 9) no attribution is produced, and previously published rankings (flare header pressure > 80% of residual mass, etc.) described a different configuration, not this run.

5. **Downstream Twin Inferences**:
   When a detection occurs, the resulting state and attribution are translated into the SCL schema tags on Gateway B:
   - `SafetySystems.model_anomaly = True`
   - `SafetySystems.model_fault_code = 9`
   - `SafetySystems.model_trip_advice = True`
   `model_trip_advice` is emitted only on an actual `ABNORMAL` transition; no fixed lead-time (such as "18 hours before ESD") is guaranteed — lead time is a measured outcome of each evaluation run, reported with honesty about what the data shows (Section 9).

---

## 6. Experiment Directory Structure

```
experiments/flare/
├── cli.py                    # Unified launcher (python3 experiments/flare <command>)
├── config.json               # Master experiment parameters, splits, and features
├── config.yaml               # Optional YAML config (preferred over JSON if present)
├── requirements.txt          # pip dependencies for this experiment
├── orchestrate.py            # Full 5-stage pipeline (used by `flare run`)
├── run_capture.py            # Stage 1: Simulation capture generating WAL archive
├── dataset.py                # Dataset extraction, standardisation, and windowing
├── model.py                  # PyTorch Autoencoder and FlareDetector inference class
├── train_detector.py           # Stage 3: Trains autoencoder & calibrates conformal thresholds
├── langchain_tools.py        # Schema-grounded read-only LangChain agent tools
├── agent.py                  # LangChain advisory agent (LLM + deterministic fallback)
├── run_demo.py                 # Demo: Live replay, twin gateway, and advisory demo
├── evaluate.py                 # Stage 4: Quantitative metric calculation & plot generation
├── evaluation_common.py        # Shared evaluation primitives (splits→windows, FAR+CP CI, baselines, provenance)
├── flare_history.jsonl.zst     # Captured 31-day plant WAL archive (~344 persist-on-change rows)
├── meta.json                 # Scenario metadata (fault onset, ESD timestamps)
├── model.pt                  # Trained PyTorch autoencoder weights
├── model_meta.json           # Serialized scaler parameters and conformal cutoffs
├── dataset.csv               # Extracted tabular dataset
├── evaluation_metrics.json   # Recorded metric verification results
└── evaluation_plot.png       # Score vs. time plot with conformal zones
```

---

## 7. End-to-End Execution Flow

Prefer the unified launcher (`python3 experiments/flare …`) from the repo root. The sections below describe individual scripts for debugging.

### Stage 1: Telemetry Capture (`run_capture.py` or `flare capture`)
- Runs isolated `s7shell` executing `simulation.as` for 30 normal days + Day 31 fault progression.
- Telemetry streams to the recording Gateway and is persisted into `flare_history.jsonl.zst`.
```bash
python3 experiments/flare capture
# or: python3 experiments/flare/run_capture.py
```
*Outputs*: `experiments/flare/flare_history.jsonl.zst` (91,941 bytes), `meta.json`.

### Stage 2: Splitting & Windowing (`dataset.py`, invoked by `orchestrate.py`)
- Extracts 20 clean process signals into sliding windows ($12 \text{ hours} \times 20 \text{ signals} = 240$ dimensions).
- Splits the **pre-fault rows chronologically** into train (60%) → calibration (20%) → held-out-normal (20%), with the scaler fitted on the train split only. Held-out rows are reserved exclusively for false-alarm-rate measurement; they never touch training, scaling, or threshold selection. (An optional second independent no-fault capture can be used instead via `dataset.heldout_source: "capture"`, but the bundled WAL comes from a deterministic replay of a hardcoded simulator seed, so a re-capture is a duplicate trajectory, not an independent one.)

### Stage 3: Training & Conformal Calibration (`train_detector.py`)
- Trains an autoencoder ($240 \to 64 \to 16 \to 64 \to 240$) on the **Train Split** (first 60% of pre-fault data).
- Evaluates reconstruction errors on the **Calibration Split** (next 20% of pre-fault data, thinned to non-overlapping blocks with `calibration_stride = 12`) and computes the empirical conformal quantile:
  - Nominal 95% threshold: $q_{\alpha}$ = the $\lceil (n+1)(1-\alpha)\rceil$-th of the calibration scores; with only 5 effective blocks available, the finite-sample rule yields $q_{\alpha} = \infty$ (never-flag regime — see Section 9).
  - Upper uncertain threshold: $q_{\text{upper}} = k \cdot q_{\alpha}$ ($k = 1.4$) — a heuristic margin, not a conformal quantity.
```bash
python3 experiments/flare train
# or: python3 experiments/flare/train_detector.py
```
*Outputs*: `model.pt`, `model_meta.json`.

### Stage 4: Quantitative Offline Evaluation (`evaluate.py`)
- Computes metrics **strictly on held-out-normal windows** (raw counts, plus non-overlapping windows with Clopper–Pearson 95% CIs), and evaluates the fault day for delay/lead only:
  - **False Alarm Rate (held-out)**: total and abnormal-only, as counts and rates; the calibration-window rates are kept separately under `calibration_rate_*` and are circular by construction.
  - **Detection Delay / Lead Time**: raw counts on the held-out FAR side; delay/lead are reported whether or not the AE beats the simple level/slope baselines on `SafetySystems.flare_header_press` (side-by-side table, Section 9).
  - **Attribution**: top residual drivers at detection, only if the detector actually flags.
```bash
python3 experiments/flare evaluate
```
*Outputs*: `evaluation_metrics.json`, `evaluation_plot.png`.

### Demo: Live Replay & LangChain Advisory Agent (`run_demo.py`)
- Spawns `sgrn_replay` (Port 18080) and twin `gateway` (Port 18082).
- Agent monitors plant state tick-by-tick and triggers operator advisories on state transitions (`NORMAL` $\to$ `ABNORMAL`).
```bash
# Fast deterministic mode (no LLM key required):
python3 experiments/flare demo

# Slower replay or LLM advisories:
python3 experiments/flare demo --speed 10
export OPENAI_API_KEY="your-key"
python3 experiments/flare demo --speed 1 --llm
```

---

## 8. Real-Time Extension: Forcing Signals via OPC-UA

To test manual signal tampering or emergency overrides:
1. Configure Gateway to expose its northbound OPC-UA listener on `port 4840`.
2. Connect any OPC-UA client (e.g., Python `asyncua`, SCADA, or UaExpert) to `opc.tcp://127.0.0.1:4840`.
3. Write forced values to `Objects/SGRN/SafetySystems/flare_header_press`.
4. **How the system reacts**:
   - The Gateway updates twin memory instantly.
   - The WebSocket stream pushes the altered tag to the detector.
   - The autoencoder reconstruction error immediately exceeds $q_{\text{upper}}$, transitioning the state to `ABNORMAL`.
   - The top-attributed signal correctly points to `SafetySystems.flare_header_press`.
   - The LangChain agent emits a critical alert on screen within the same tick.

---

## 9. Quantitative Verification Summary

Actual metrics from the latest full pipeline run with the bundled WAL (`python3 experiments/flare run`; see `evaluation_metrics.json` for raw counts, the Clopper–Pearson interval, and provenance — seed, git commit, config hash — on every number below).

### Held-out false-alarm behavior (AE detector)

| Metric | Measured Value | Notes / Requirement | Result |
|:---|:---|:---|:---|
| **Calibration windows (stride-1)** | `55` | sets $q_{\alpha}$ only — **circular for FAR** | Valid |
| **Effective calibration blocks (stride = 12)** | `5` | finite 95% quantile needs ≥ 19 | **Insufficient → $q_{\alpha} = \infty$** |
| **Held-out normal windows (stride-1)** | `56` flagged `0` | raw counts on held-out data | FAR point estimate **0.00%** |
| **Held-out independent windows (stride = 12)** | `5`, flagged `0` | Clopper–Pearson 95% for 0/5 | **[47.8%, 100%]** — the upper bound, not 0.00%, is the defensible number |
| **Detector regime** | never-flag | $q_{\alpha} = \infty$: by the conservative finite-sample conformal rule the detector cannot emit `UNCERTAIN`/`ABNORMAL` | **Honest negative result, reported as-is** |

### Fault-day detection performance (AE vs. simple baselines)

The baselines use only `SafetySystems.flare_header_press`: a window **level** (mean) and a window **slope** (mean per-tick difference), each thresholded at train-mean $+ k\cdot$std with $k$ chosen so the *calibration*-window FAR $\le \alpha = 0.05$ — no peeking at held-out or fault-day data. Delay is hours from fault onset to the first flag; lead is hours from that flag to the 90 bar ESD trip (2000-01-31 22:00).

| Detector | Calibration FAR ($\le$ 5% budget) | Held-out FAR (abnormal-only) | Delay | Lead before ESD |
|:---|:---|:---|:---|:---|
| **Conformal AE** | n/a (never flags) | 0.00% (0/56) | **none — no detection** | none |
| **Baseline level (window mean), k = 2.5** | 3.64% (2/55) $\checkmark$ | 0.00% (0/56) | **0.0 h** | **19.0 h** |
| **Baseline slope (mean per-tick diff), k = 1.75** | 3.64% (2/55) $\checkmark$ | 7.14% (4/56) | **0.0 h** | **19.0 h** |

**Plain statement:** the simple level threshold detects the fault as early as — and in this run strictly *better* than — the conformal autoencoder. The AE never flagged at all (its 0.00% FAR is trivially satisfied, not earned), so the baselines win on this scenario.

### Timeline anchors (verified)

| Anchor | Value |
|:---|:---|
| Fault onset | `2000-01-31 03:00:00` (Day 31, Hour 3) |
| Emergency Shutdown (ESD) | `2000-01-31 22:00:00` (feed pressure $> 90.0$ bar) |

### Limitations

- **The held-out split is not an independent run.** The bundled WAL replays a deterministic simulator with a hardcoded seed, so train/calibration/held-out rows come from the same trajectory; the chronological split blocks direct contamination but not cross-split autocorrelation. The `capture --no-fault` option exists, but a re-captured "normal" run is a duplicate, not new physics.
- **The dataset is small and sparse.** Persist-on-change recording yields ~344 rows over 31 days (2–4 h gaps), not the 742 regular hourly rows an earlier draft claimed. With 12-row windows that leaves only 199/66/67 rows and 5 effective calibration blocks — too few for a finite 95% conformal quantile, hence the never-flag regime.
- **Delay/lead of 0.0 h / 19.0 h are artifacts of sparse sampling,** not evidence of faster detection; the first flagged window simply ends at the fault-onset tick.
- **The stride-1 FAR point estimate (0/56) is over-optimistic** because overlapping windows share 11 of 12 timesteps; the stride-12 count (0/5) and its CI upper bound are the defensible numbers, and both are far from the nominal 5% error budget.
- **`q_upper = k·q_alpha` is a heuristic,** with no conformal coverage guarantee, and attribution output exists only when the detector flags (currently never).
