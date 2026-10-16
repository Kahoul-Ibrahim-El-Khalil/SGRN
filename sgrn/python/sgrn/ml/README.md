# `sgrn.ml` — Industrial Process Machine Learning & Conformal Manifold Anomaly Detection

`sgrn.ml` provides a domain-separated, hardware-accelerated machine learning and statistical uncertainty framework designed specifically for continuous industrial plant telemetry, Siemens S7 PLC memory integration, and digital twins.

> **Control boundary.** This package produces predictions, scores and
> evidence for downstream decision-making. It never writes to PLCs and
> never bypasses the control layer: the only write path anywhere near it
> is an explicit user call (`AutoMLTrainer.saveToDatastore(gateway, …)`,
> `ActionCompiler` after a passed `SchemaChecker`), never an automatic
> side effect of training or inference.

> **Naming.** Public functions/methods are `camelCase`, classes
> `PascalCase`, module constants `UPPER_SNAKE_CASE`. Parameters keep
> their long-published `camelCase` spelling (demos, experiments and
> tests call them by keyword) — renaming them would break the API for
> no behavioural gain; every public function additionally ships a
> `snake_case` alias. Removed pre-convention spellings
> (`train_baseline`, `calibrate_conformal`, `evaluate_sample`, `to_json`)
> had zero callers; the snake_case function aliases stay under the
> compatibility policy.

---

## 1. Domain Architecture

The package is partitioned into single-responsibility domains:

```
sgrn/python/sgrn/ml/
├── device.py       # Hardware acceleration (NVIDIA CUDA, AMD ROCm/HIP, Apple Silicon MPS, CPU)
├── schema.py       # SCL schema parser, semantic dimension taxonomy (#UNIT/#RANGE) & model auto-configuration
├── transform.py    # Telemetry filtering, standardisation (Scaler), timeline splitting & sliding windows
├── manifold.py     # PyTorch WindowAutoencoder, ProcessManifoldDetector & split-conformal calibration
├── pipeline.py     # Composed end-to-end training (fitManifoldPipeline) & evaluation (evaluateManifoldSequence)
├── dataset.py      # Binary WAL (.bin.zst) and JSONL streaming replay readers
├── anomaly.py      # Lightweight point-sample conformal anomaly detector
├── diagnostics.py  # Statistical process monitoring (PCA) & sample attribution
└── trainer.py      # AutoML tabular training helpers
```

---

## 2. Submodule Reference

### `device.py` — Hardware Acceleration & Accelerator Discovery

Enables transparent hardware acceleration across heterogeneous compute environments without code changes:

- **AMD ROCm / HIP**: Discovered via `torch.version.hip`. Automatically utilizes HIP kernel dispatches and pinned memory.
- **NVIDIA CUDA**: Discovered via `torch.cuda.is_available()`.
- **Apple Silicon (MPS)**: Discovered via `torch.backends.mps.is_available()`.
- **Host CPU**: High-efficiency fallback.

#### Key Functions
```python
from sgrn.ml import getHardwareInfo, resolveDevice

info = getHardwareInfo()
# Example (AMD):    {'platform': 'AMD ROCm', 'backend': 'HIP 6.1', 'deviceType': 'cuda', ...}
# Example (NVIDIA): {'platform': 'NVIDIA CUDA', 'backend': 'CUDA 12.4', 'deviceType': 'cuda', ...}

device = resolveDevice()  # Returns torch.device('cuda') or torch.device('cpu')
```

---

### `schema.py` — SCL Schema Parser & Model Auto-Configuration

Source priority: gateway `GET /registry` JSON or `.bin.zst` embedded schema first (`signalsFromRegistry` / `configureModelFromRegistry` — real offsets, resolved enums/aliases, authoritative `dimension`/`min`/`max`/`endianness`), SCL text (`parseSclSchema`) only as the offline fallback. DTL/LDTL fields are reported as `timestampFields` for direct ns normalisation, never as features.

Parses Siemens S7 SCL (`.scl`) source files directly, establishing the SCL schema as the **single source of truth** for both PLC memory layout and AI model architecture:

- **UDT & DB Expansion**: Recursively parses UDT structures (`FlowMeter`, `ValveActuator`, `SieveBed`) and multidimensional arrays to discover fully qualified signal paths (e.g. `InletSeparation.feed_flow.rate`, `AdsorberTowers.towers[0].bed_dp`).
- **Semantic Pragma Taxonomy**: Maps `#UNIT`/`#RANGE`/`#ENUM` pragmas (case-insensitive) to physical thermodynamic domains:
  - `bar`, `psi`, `kPa` $\to$ **Pressure**
  - `°C`, `K` $\to$ **Temperature**
  - `dam³/h`, `m³/h`, `kg/h` $\to$ **Flow**
  - `%`, `pct` $\to$ **Percent** (dimensionless ratio; Level vs. valve/speed setpoint comes from the signal name, never the bare unit)
  - `rpm` $\to$ **Rotational Speed**
  - `mm/s` $\to$ **Vibration**
  - `MJ/m³` $\to$ **Energy**
  - `ppm`, `ppmv` $\to$ **Composition**
- **Automated Topology Derivation**: Automatically calculates neural autoencoder layer dimensions ($W \times (S + C) \to \text{hidden} \to \text{latent}$) and discovers discrete operating context (`Bool` flags, `#ENUM` modes incl. named scalar aliases like `PlantMode`) alongside continuous features. Model-feedback registers, trip setpoints, forecasts/specs and ESD logic are excluded by role (`_EXCLUDE_*`), not by plant; extend via `excludeSignals`.
- **Discrete context, not discrete targets**: context signals condition the encoder and define standardisation strata; they are never reconstructed, so enum code distance never pollutes the anomaly score.

#### Key Functions
```python
from sgrn.ml import parseSclSchema, configureModelFromSchema

# Parse raw SCL AST
parsed = parseSclSchema("path/to/schema.scl")

# Auto-configure feature space, dimensions, and model geometry
cfg = configureModelFromSchema("path/to/schema.scl", windowSize=12)
# cfg["features"]           -> List of continuous process signals
# cfg["featureDimensions"]  -> Map of signal -> physical dimension
# cfg["featureSubsystems"]  -> Map of signal -> plant DB name
# cfg["contextFeatures"]    -> List of discrete conditioning signals (Bool/#ENUM)
# cfg["contextSpecs"]       -> Map of signal -> {"kind": "bool"|"enum", "categories": [...]}
# cfg["modelConfig"]        -> Calculated input_dim, hidden_dim, latent_dim
```

---

### `transform.py` — Telemetry Preprocessing & Windowing

Handles the data engineering pipeline from raw SCADA tables or WAL streams into normalized sliding temporal tensors:

- **`Scaler`**: Dataclass providing Z-score standardisation ($\widetilde{\mathbf{x}} = \frac{\mathbf{x} - \boldsymbol{\mu}}{\boldsymbol{\sigma}}$) fitted strictly on pre-fault baseline data to prevent temporal data leakage, with near-zero variance safeguards.
- **`StreamingStats`**: Welford online mean/variance (NaN-skipping, exactly mergeable, bit-parity with `fitProcessScaler`) — the scaler without the matrix.
- **Discrete context encoding**: `extractRawContextColumns` / `encodeContextMatrix` one-hot encode `Bool`/`#ENUM` modes per timestep (enums carry an extra unknown/other column); `inferContextSpecs` derives specs from nominal training columns when no schema is present.
- **Mode-stratified standardisation**: `fitStratifiedStats` / `applyStratifiedStats` z-score each timestep by its own mode's nominal mean/std, so a value nominal in one mode but impossible in another scores as a large residual in any model. Under-sampled strata fall back to global statistics.
- **`filterProcessTimeline`**: Slices data by operational cycle boundaries or timestamp ranges.
- **`extractFeatureMatrix`**: Validates signal presence, casts to `float32`, and applies forward/backward fill imputation.
- **`splitProcessTimeline`**: Chronologically partitions nominal pre-fault data into training (70%) and conformal calibration (30%), isolating post-fault test data.
- **`createSlidingWindows`**: Constructs temporal sliding observation arrays $(N, W, S)$ where $W$ is window length and $S$ is signal count.

#### Key Functions
```python
from sgrn.ml import (
    Scaler,
    filterProcessTimeline,
    extractFeatureMatrix,
    extractContextMatrix,
    fitStratifiedStats,
    splitProcessTimeline,
    fitProcessScaler,
    createSlidingWindows,
)
```

---

### `manifold.py` — Manifold Autoencoder & Conformal Detector

Implements the physical manifold projection operator and split-conformal statistical uncertainty bounds:

- **`WindowAutoencoder` (`nn.Module`)**: Symmetric MLP autoencoder with parameterized bottleneck compressing flattened sliding windows ($W \cdot (S + C) \to 64 \to 16 \to 64 \to W \cdot S$), where $C$ is the one-hot discrete-context width. Context conditions the encoder; the decoder reconstructs continuous signals only.
- **`ModePredictor` (`nn.Module`)**: Second head trained on the same nominal windows, answering "which mode does the physics imply?" from the continuous window *alone* (feeding it the actual codes would be a trivial copy). Per-Bool BCE / per-enum cross-entropy nonconformity with its own conformal bounds (`mode_mismatch` names the disagreeing switch).
- **`ProcessManifoldDetector`**: Production detector wrapper that:
  - Projects incoming windows through the neural autoencoder on GPU/CPU.
  - Computes orthogonal reconstruction error $e(\mathbf{x}) = \frac{1}{W \cdot S} \|\widetilde{\mathbf{x}} - \widehat{\mathbf{x}}\|^2$ on mode-stratified residuals.
  - Evaluates conformal 3-state uncertainty:
    - **`normal`**: $e \le q_\alpha$ (within tolerance tube $\mathcal{T}_\alpha$).
    - **`uncertain`**: $q_\alpha < e \le k \cdot q_\alpha$ (boundary regime; soft operator warning without immediate plant trip).
    - **`abnormal`**: $e > k \cdot q_\alpha$ (statistically significant manifold departure; trip advisory).
  - Escalates unseen operating modes / unknown categories to at least `uncertain` (`unseen_mode`, `context_unknown`, `context_mode` in every verdict).
  - Holds per-signal conformal bounds alongside the window-mean bound, so single-sensor faults are not diluted away (`abnormal_signals`).
  - Decomposes projection residual across **Physical Dimensions** and **Plant Subsystems**.
  - Supports high-throughput vectorized GPU batch inference via `detectBatch()`.
- **`trainManifoldDetector`**: Trains autoencoder on nominal baseline data and calibrates empirical quantile $q_\alpha = \text{Quantile}_{\frac{\lceil(n+1)(1-\alpha)\rceil}{n}}(\{s_i\})$ on held-out calibration split, thinned to non-overlapping blocks (`calibrationStride`, default = window length) for approximate exchangeability.

#### Key Functions
```python
from sgrn.ml import ProcessManifoldDetector, trainManifoldDetector

detector = trainManifoldDetector(
    X_train=X_train,
    X_cal=X_cal,
    scaler=scaler,
    features=features,
    config=model_config,
    device="cuda",  # AMD ROCm or NVIDIA CUDA
)

# Single window detection
result = detector.detect(raw_window)

# Vectorized GPU batch scoring
batch_results = detector.detectBatch(raw_windows, batchSize=512)
```

---

### `pipeline.py` — Composed End-to-End Orchestration

Provides high-level functional composition tying together schema parsing, data splitting, scaling, training, calibration, and inference into clean single-function calls:

- **`fitManifoldPipeline(...)`**: End-to-end pipeline that takes a dataset (DataFrame or array) and an SCL schema path, automatically deriving continuous features, discrete context, mode strata, scaling, windowing, and fitting the detector (`useContext=False` restores the legacy continuous-only model; `stratifyOn`/`minStratumSamples` control mode stratification; `trainModeHead=False` disables the physics→mode head).
- **`fitManifoldPipelineStream(reader, ...)`**: Same model and thresholds in O(window) memory — rows stream from `BinaryDatasetReader`/`JsonlDatasetReader.iterRows` through four chronological passes (count+cutoff, Welford stats, ring-buffer training epochs with shuffle buffer, streaming calibration). `schemaSource` defaults to the reader's embedded schema, so an archive trains a detector directly. `splits` carries counts, not window arrays (never materialised).
- **`evaluateManifoldSequence(...)`**: Evaluates an entire continuous timeline or test split with GPU-accelerated batch scoring (takes matching `contextData` when the detector uses context).
- **`inferStateAt(detector, data, timestamp, ...)`**: "What was happening then?" — the window ending at a wall-clock time through `detect`, from a DataFrame or straight from an archive reader (O(window) streaming).
- **`predictFeature(detector, window, feature, ...)`**: Manifold-constrained nowcast of one signal from all the others (masked reconstruction + conformal interval); fault windows land far outside with large `surprise`.

#### Key Functions
```python
from sgrn.ml import fitManifoldPipeline, evaluateManifoldSequence

# One-call pipeline execution
pipeline = fitManifoldPipeline(
    data=df,
    schemaSource="schema.scl",
    windowSize=12,
    cycleStart="2000-01-01 00:00:00",
    faultOnset="2000-01-31 03:00:00",
)

detector = pipeline["detector"]
eval_res = evaluateManifoldSequence(detector, test_df)
```

---

### `dataset.py` — WAL Archive & Binary Storage Readers

Provides stream readers for SGRN persistence formats:

- **`BinaryDatasetReader`**: Decodes high-performance binary WAL (`.bin.zst`) frames (full anchor frames, delta runs, and control frames) with CRC32 verification.
  - `readDataset()` — legacy `(timestamps_ms, names, float32 matrix)` view. Numeric fields decode with their schema byte order; DTL/LDTL timestamp fields read as NaN here.
  - `readPandas()` — preferred: builds the DataFrame **directly from WAL frames, no CSV intermediate** (faithful dtypes: float32/64, bool, int32, datetime64[ns] for DTL/LDTL, plus a `timestamp` column from the frame clock). The `sgrn_dataset` CSV export stringifies floats and bool-maps by string compare — treat CSV as interchange, not a pipeline stage.
- **`JsonlDatasetReader`**: Decodes gateway JSONL replay archives (`.jsonl.zst` / `.jsonl`) with carried-forward state, file-local leaf dictionaries and nested/legacy shapes — `readPandas()` straight into a DataFrame, same as the binary path.
- **`DatasetReader`**: Reads `sgrn_dataset` manifest + CSV distribution bundles (interchange format).

---

### `diagnostics.py` & `trainer.py` — Process Monitoring & AutoML

- **`fitMonitorAndDiagnoser`**: Fits normal-only PCA monitors with scikit-learn random forest classifiers for baseline comparison.
- **`explainSample`**: Explains point feature contributions relative to normal operating envelopes.
- **`AutoMLTrainer`**: Tabular model training utilities.

---

## 3. Mathematical Foundations

### The Physical Manifold Hypothesis
Continuous industrial plants operate under strict physical laws:
$$\sum \dot{m}_{\text{in}} = \sum \dot{m}_{\text{out}} + \frac{dm_{\text{cv}}}{dt} \quad (\text{Mass Conservation})$$
$$\frac{dU}{dt} = \dot{Q} - \dot{W} + \sum \dot{m}h \quad (\text{Energy Conservation})$$
$$P = \frac{RT}{v-b} - \frac{a(T)}{v(v+b) + b(v-b)} \quad (\text{Peng-Robinson EOS})$$

Because of these coupled constraints, nominal telemetry observations $\mathbf{x} \in \mathbb{R}^D$ are confined to a smooth lower-dimensional manifold:
$$\mathcal{M} \subset \mathbb{R}^D, \quad \dim(\mathcal{M}) = d \ll D$$

### Orthogonal Residual Metric
The autoencoder acts as a non-linear orthogonal projection operator:
$$\widehat{\mathbf{x}} = \operatorname{Proj}_{\mathcal{M}}(\widetilde{\mathbf{x}}) = f_{\text{dec}}(f_{\text{enc}}(\widetilde{\mathbf{x}}))$$

Under normal operation, $\widetilde{\mathbf{x}} \in \mathcal{M}$ and reconstruction error is near-zero. When a physical fault occurs (e.g. hydrate plug, blocked flare, valve stiction), physical conservation is violated, forcing the operating point off the manifold:
$$e(\mathbf{x}) = \operatorname{dist}^2(\widetilde{\mathbf{x}}, \mathcal{M}) = \frac{1}{D} \|\widetilde{\mathbf{x}} - \widehat{\mathbf{x}}\|^2$$

### Why Sliding Windows: Dynamical Phase Space & Conformal Exchangeability

A single instantaneous time step $\mathbf{x}_t \in \mathbb{R}^D$ provides only a static snapshot of sensor voltages and process levels. In an industrial dynamical system, instantaneous points cannot disambiguate velocity, acceleration, thermal lag, or hysteresis. 

To overcome this, SGRN constructs **temporal sliding observation tensors** $\mathbf{X}_w = [\mathbf{x}_{t-W+1}, \dots, \mathbf{x}_t] \in \mathbb{R}^{W \times D}$ ($W = 12\text{ hours}$):

1. **Phase-Space Trajectory Reconstruction (Takens' Delay Embedding)**:
   According to delay-coordinate embedding theorems, stacking temporal lags of observable process states reconstructs an unobserved dynamical state space diffeomorphic to the true underlying physical manifold. The autoencoder therefore learns the *trajectory* and rate of change of the plant rather than isolated points.
2. **Cyclic Stationarity Over Full Operational Limits**:
   The molecular sieve dehydration towers operate cyclically (adsorption, thermal regeneration at $290^\circ\text{C}$, and cooling). Setting $W = 12\text{h}$ covers the complete switching period, converting periodic non-stationarity into cyclically stationary limit cycles in $\mathbb{R}^{W \times D}$.
3. **Preserving Conformal Quasi-Exchangeability**:
   Conformal prediction relies on the exchangeability of calibration scores. Consecutive raw seconds are strongly autocorrelated and non-exchangeable. By grouping observations into full-cycle windows and evaluating them across chronological (non-random) partitions, adjacent window scores behave as quasi-independent draws from the ergodic nominal distribution, preserving empirical coverage guarantees ($0.00\%$ abnormal false alarms on held-out calibration data).
4. **Transient Filtering vs. Persistent Physical Excursions**:
   A momentary sensor glitch or valve travel flutter is transient and localized to a single tick. A genuine cyber-physical failure (e.g. flare header blockage ramping pressure at $+0.15\text{ bar/tick}$) produces a sustained, monotonic trajectory drift across the entire 12-hour window, eliminating spurious trip alerts.

---

### Multi-Dimensional Deviation Attribution

In the ambient telemetry space $\mathbb{R}^D$, each **dimension $d \in \{1, \dots, D\}$ represents a distinct process feature/signal** declared in the SCL schema.

The reconstruction residual vector $\mathbf{r} = \widetilde{\mathbf{x}} - \widehat{\mathbf{x}} \in \mathbb{R}^D$ decomposes directly along each feature dimension:
$$e_d = \frac{1}{W} \sum_{t=1}^W (X_{w, t, d} - \widehat{X}_{w, t, d})^2$$

Because the schema defines the coordinate space, the detector calculates the exact deviation in each feature dimension:
- **`primary_dimension`**: `SafetySystems.flare_header_press` (the top deviating feature dimension)
- **`dimension_breakdown`**: The vector of deviations $\{e_d\}_{d=1}^D$ showing each feature's contribution to total manifold error.
- **`primary_subsystem`**: `SafetySystems` (the specific plant datablock/device owning the primary dimension)

This feature-level deviation vector is directly consumable by downstream reasoning agents or LLMs to pinpoint which physical device or control loop requires adjustment.

---

## 4. Closing the Cyber-Physical Feedback Loop

```
[ 1. SCL Schema (schema.scl) ]
       │ Defines S7 DB memory offsets, tags, and continuous feature dimensions
       ▼
[ 2. SGRN Gateway (s7shell) ]
       │ Streams live process telemetry into WAL and memory mirrors
       ▼
[ 3. sgrn.ml Auto-Configuration ]
       │ Derives feature dimensions and PyTorch autoencoder topology
       ▼
[ 4. GPU Manifold Inference (AMD ROCm / CUDA) ]
       │ Projects telemetry onto manifold M, scores conformal bounds
       ▼
[ 5. Feature Dimension Attribution ]
       │ Pinpoints: SafetySystems.flare_header_press (+2.84 bar drift)
       ▼
[ 6. Gateway Closed-Loop Feedback ]
       │ Writes diagnosis directly back into Gateway DB6 (SafetySystems):
       │   - SafetySystems.model_anomaly = TRUE
       │   - SafetySystems.model_trip_advice = TRUE
       │   - SafetySystems.model_fault_code = 9 (FLARE_BLOCKED)
       ▼
[ 7. Plant Safety Actuation ]
       └ Controlled venting / protective trip initiated 18 hours before ESD
```
