# SGRN Machine Learning & Digital Twin Bridge (`sgrn_ml`)

The `sgrn_ml` component provides the core C++23 machine learning runtime, feature extraction engine, discrete time-grid resampler, zero-copy columnar dataset architecture (`.sgrnb`), and live Digital Twin inference bridge (`TwinBridge`) for industrial edge automation.

---

## 1. Architectural Highlights

### 1.1 Asymmetric Amortized Endianness
In industrial PLC environments (Siemens S7-1500 / S7-300), fieldbuses transmit telemetry and accept setpoints in **Big-Endian** wire order. Modern edge servers and GPUs execute on **Little-Endian** architectures (x86_64, AArch64).
- **Previous Bottleneck**: Gateways that maintained twin buffers in wire format were forced to repeatedly swap bytes for every scalar read by northbound consumers (OPC-UA, WebSockets, REST APIs, ML tensors).
- **SGRN Solution**: Standardizes all in-memory Digital Twin buffers (`PlcState`, `DbSnapshot`) to host **Little-Endian**. Wire byte-swapping is amortized to single bulk operations strictly at network ingress and egress. Northbound subscribers and PyTorch tensor loaders consume memory directly with **zero per-scalar byte-swap penalty**.

### 1.2 Ground-Truth Machine Clocks vs. OS Receive Jitter
Telemetry arrives at edge gateways with non-deterministic network transport and OS thread scheduling jitter ($\pm 1 \text{ to } 10\,\text{ms}$).
- Ground-truth process time is determined by Siemens S7 DTL (`Date and Time Long`) clocks written directly into DataBlocks by the PLC cycle.
- The ML pipeline extracts ground-truth 64-bit nanosecond epochs via `s7codec::decodeDtlToNanoseconds()` and routes prediction timestamps back into DataBlocks via `TwinOutputTarget::Timestamp` with zero floating-point truncation.

---

## 2. Discrete Time-Grid Resampler (`TimeGridResampler`)

Neural networks (MLPs, LSTMs, Transformers) and physical state estimators require telemetry sampled on a strictly uniform discrete time grid:
$$t_k = t_0 + k \cdot \Delta t$$

Industrial PLCs, however, emit updates asynchronously, at varying cadences across DataBlocks (e.g., fast dynamics at 10ms, slow loops at 100ms), or in delta-compressed bursts.

### 2.1 Resampling Engine
`TimeGridResampler` synchronizes asynchronous streams into discrete intervals $\Delta t$ using typed accumulators:

- **Analog Sensors (Temperatures, Pressures, Flows)**: `#AGG("MEAN")` (default for `Real`/`LReal`). Computes arithmetic mean over all updates inside bucket $k$. Optional `#AGG("MIN")` or `#AGG("MAX")` for peak detection.
- **Digital States & Alarms**: `#AGG("OR")` (default for `Bool`). Applies logical OR across the bucket so momentary transient trips are never lost to subsampling.
- **Actuators & Setpoints**: `#AGG("LAST")`. Retains the final settled position within the interval.
- **Zero-Order Hold Forward-Filling (`forward_fill = true`)**: Across communication gaps or slow-cadence loops, previous valid values are carried forward, preventing NaN injection without distorting process dynamics.

---

## 3. Columnar Binary Dataset Format (`.sgrnb`)

Traditional ML data preparation relies on intermediate CSV files. In high-frequency industrial environments (e.g. 100 tags at 100Hz = 10,000 floats/sec), CSV generation causes multi-gigabyte disk blowup and CPU parsing bottlenecks.

`.sgrnb` is a direct, zero-copy, memory-mapped binary dataset format:

```text
+-------------------------------------------------------------+
| MAGIC: 'S', 'G', 'R', 'N', 'B', '0', '1', 0x00 (8 bytes)    |
+-------------------------------------------------------------+
| Header: manifest_len (uint32_t), num_rows (uint64_t),       |
|         num_features (uint32_t), num_labels (uint32_t),    |
|         step_ns (uint64_t), flags (uint32_t)                |
+-------------------------------------------------------------+
| Embedded ModelManifest JSON (Schema, FeatureSpec, Labels)   |
+-------------------------------------------------------------+
| Padding (0 to 63 bytes) to 64-byte boundary                 |
+-------------------------------------------------------------+
| Timestamp Column: [t_0, t_1, ..., t_{N-1}] (uint64_t * N)  |
+-------------------------------------------------------------+
| Feature Column 0: [f_{0,0}, f_{0,1}, ...] (float32 * N)     |
+-------------------------------------------------------------+
| Feature Column 1: [f_{1,0}, f_{1,1}, ...] (float32 * N)     |
+-------------------------------------------------------------+
| ...                                                         |
+-------------------------------------------------------------+
| Label Column 0:   [l_{0,0}, l_{0,1}, ...] (float32 * N)     |
+-------------------------------------------------------------+
```

### Key Technical Properties:
- **64-Byte Cache-Line Alignment**: Guarantees optimal SIMD vector loading (AVX-512, ARM Neon) and direct DMA transfer to GPU VRAM.
- **Zero-Copy Ingestion**: `ColumnarDatasetReader::open()` calls `mmap(PROT_READ)`. Floats are accessed as contiguous `std::span<const float>` directly from disk cache.
- **Instant Tensor Construction**: PyTorch tensors are constructed via `torch::from_blob(col.data(), {N}, torch::kFloat32)` in sub-millisecond time.

---

## 4. Live Digital Twin Inference Bridge (`TwinBridge`)

The `TwinBridge` binds trained machine learning models to live PLC memory segments:

```mermaid
flowchart LR
    subgraph TWIN["PlcState (Host Little-Endian)"]
        IN_DB["Input DataBlocks\n(e.g. DB10: Temperatures, Flows)"]
        OUT_DB["Output DataBlocks\n(e.g. DB30: Predictions, Alarms, DTL)"]
    end

    subgraph BRIDGE["TwinBridge Engine"]
        EXTRACT["Feature Extraction\n(MinMax / Z-Score Norm)"]
        INFER["PredictionEngine\n(Torch MLP / ONNX / Custom)"]
        EVAL["Threshold Evaluator\n(Warning / Alarm Bits)"]
    end

    IN_DB -->|Direct Memory Read| EXTRACT
    EXTRACT --> INFER
    INFER --> EVAL
    EVAL -->|PredictionValue (Float)| OUT_DB
    EVAL -->|AlarmBit (Bool)| OUT_DB
    INFER -->|Timestamp (64-bit ns -> S7 DTL)| OUT_DB
```

### Output Route Targets:
- `TwinOutputTarget::PredictionValue`: Continuous scalar predictions (e.g., thermal power, estimated load).
- `TwinOutputTarget::AnomalyScore`: Raw distance metric or reconstruction error.
- `TwinOutputTarget::AlarmBit`: Boolean alarm raised when residual exceeds configured warning/alarm threshold.
- `TwinOutputTarget::Timestamp`: Directly encodes the live nanosecond tick epoch into a 12-byte Siemens S7 DTL without passing through float32 precision truncation.

---

## 5. Verification & Testing

Run the full in-tree test suite:

```bash
# Discrete time-grid resampler tests (aggregations, multi-clock, forward-fill)
./.build/linux-static-release/sgrn/lib/ml/time_grid_resampler_test

# Columnar dataset tests (write, mmap read, span access, 64-byte alignment)
./.build/linux-static-release/sgrn/lib/ml/columnar_dataset_test

# End-to-end pipeline test (telemetry gen -> resample -> .sgrnb -> train -> live gateway)
./.build/linux-static-release/sgrn/lib/ml/pipeline_e2e_test
```
