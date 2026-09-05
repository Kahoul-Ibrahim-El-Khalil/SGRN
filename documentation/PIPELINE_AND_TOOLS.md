# SGRN Data & Machine Learning Pipeline, Conventions, and Tooling Guide

This document defines the architectural abstractions, schema conventions, dataset extraction pipelines, AutoML workflows, CLI entry points, and native desktop UI capabilities in the SGRN platform.

---

## 1. Overview & Architectural Abstractions

SGRN provides a deterministic end-to-end industrial data engine and digital twin framework:

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│ PRIMARY INDUSTRIAL EDGE                                                                 │
│                                                                                         │
│  S7 / Modbus / EtherNet-IP PLC  ──►  sgrn_gateway (Primary)  ──►  s7shell (Soft-PLC)   │
│                                           │ (Zstd WAL historian)                        │
└───────────────────────────────────────────┼─────────────────────────────────────────────┘
                                            │ Streaming `.jsonl.zst` Archives
                                            ▼
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│ C++ FEATURE EXTRACTION & DATASET ENGINE (`sgrn_dataset`)                                │
│                                                                                         │
│ • Decompresses Zstd streaming archives                                                 │
│ • Reconstructs dense timestamped state deltas                                           │
│ • Validates SCL schema `#UNIT("...")` metadata & categorical enum taxonomy              │
│ • Outputs normalized `dataset.csv` + `manifest.json`                                    │
└───────────────────────────────────────────┼─────────────────────────────────────────────┘
                                            │ (`manifest.json` + `dataset.csv`)
                                            ▼
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│ PYTHON HIGHER-LEVEL AutoML TOURNAMENT (`sgrn.ml`)                                      │
│                                                                                         │
│ • `DatasetReader`: Imputes initial NaNs, encodes categorical factors                    │
│ • `AutoMLTrainer`: Runs model competition (Ridge, RandomForest, XGBoost, IsolationForest)│
│ • Selects champion model and exports serialized weights (`model_meta.json` / ONNX)      │
└───────────────────────────────────────────┼─────────────────────────────────────────────┘
                                            │ Live Timestamp Evolution Predictions
                                            ▼
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│ DIGITAL TWIN MIRROR & REAL-TIME COMPARISON                                              │
│                                                                                         │
│ • Model predictions posted to Prediction Gateway (`port 8082`)                         │
│ • Side-by-side Dual Desktop Webviews (`--gui`) display live state & residuals           │
└─────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Schema Conventions & `#UNIT` Directives

SGRN enforces strict separation between **signal names** and **physical units**:

1. **Normalized Signal Names**: Tag names in SCL schemas and simulation scripts must be unit-agnostic (e.g. `thermal_power`, `core_inlet_temp`, `przr_pressure` rather than `thermal_power_mw` or `temp_c`).
2. **`#UNIT("...")` SCL Directives**: Physical units are declared directly in SCL schemas via `#UNIT("...")` annotations.

### SCL Schema Specification Example (`schema.scl`)

```scl
DATA_BLOCK "Reactor"
{
    VERSION: '0.1'
}
STRUCT
    thermal_power   #UNIT("MW")   : REAL := 3400.0;  // Thermal output power
    core_inlet_temp #UNIT("degC") : REAL := 290.0;   // Reactor inlet temperature
    przr_pressure   #UNIT("bar")  : REAL := 155.0;   // Pressurizer pressure
    control_rod_pos #UNIT("%")    : REAL := 100.0;   // Rod position
END_STRUCT
END_DATA_BLOCK
```

- **C++ Manifest Generator (`sgrn_dataset`)** extracts `#UNIT` metadata into `manifest.json`.
- **Python Bindings (`sgrn.ml.DatasetReader`)** preserve `#UNIT` metadata for automated feature scaling and unit conversions.

---

## 3. Core Tools & Binary Entry Points

| Tool Binary | Location / Path | Purpose & Responsibilities |
|---|---|---|
| **`gateway`** | `.prefix/bin/gateway` | Passive multi-protocol aggregator & Digital Twin server (S7, Modbus, EtherNet/IP, OPC UA, HTTP, WebSocket). |
| **`s7shell`** | `.prefix/bin/s7shell` | Soft-PLC simulator & active control engine executing AngelScript logic (`plc_logic.as`). |
| **`sgrn_dataset`** | `.prefix/bin/sgrn_dataset` | High-performance C++ dataset generator and archive converter (`.bin.zst` <-> `.jsonl.zst` <-> `dataset.csv`). |
| **`sgrn_replay`** | `.prefix/bin/sgrn_replay` | Gateway history archive replayer; initializes protocol interfaces for Northbound subscribers and streams archive frames. |
| **`demo_datapipeline.py`** | `./demo_datapipeline.py` | End-to-end test harness: records live history, extracts datasets, trains AutoML models, and runs twin predictions. |
| **`demo.py`** | `./demo.py` | Interactive post-compilation simulation harness launcher. |

---

## 3.1 `sgrn_dataset` CLI Utility Guide

`sgrn_dataset` provides standard Unix CLI syntax for dataset processing, binary conversion, and decompression.

### Synopsis
```bash
# Dataset processing (extract feature CSV and ML manifest)
sgrn_dataset -i <INPUT_DIR> -s <SCHEMA.scl> [-o <DATASET.csv>] [-m <MANIFEST.json>]

# Archive conversion (.bin.zst -> .jsonl.zst; jsonl -> binary is not implemented)
sgrn_dataset -i <ARCHIVE.bin.zst> -f jsonl [-o <OUTPUT.jsonl.zst>]

# Archive merge (directories, files, or mixed; order preserved for explicit files)
sgrn_dataset --merge <MERGED.zst> -i <DIR> [-i <FILE>...]

# Positional usage
sgrn_dataset archive.bin.zst -f jsonl
```

### Options & Flags
- `-i, --input <FILE|DIR>`: Path to input telemetry file or archive directory. Supports positional argument.
- `-o, --output <FILE>`: Path to output destination file (CSV or converted archive).
- `-f, --format <FORMAT>`: Target conversion format (`jsonl`, `binary`, `csv`).
- `-s, --scl <FILE>`: Path to SCL schema file (`.scl`).
- `-c, --convert <FILE>`: Shortcut flag to convert target archive file.
- `-d, --decompress` / `-z, --compress`: Explicit decompression or compression flags.
- `-m, --manifest <FILE>`: Target ML manifest JSON file output path. Default: `manifest.json`.

---

## 4. Native Desktop GUI Mode (`--gui`)

The `gateway` binary features a cross-platform desktop UI capability:

```bash
# Launch Gateway with Native Desktop GUI Window
.prefix/bin/gateway sgrn/gateway/simulations/nuclear/gateway.json --gui
```

### Key Behaviors:
- **Cross-Platform**:
  - **Linux**: Spawns Chromium/Firefox in standalone application window mode (`--app=http://127.0.0.1:<PORT>/`).
  - **Windows**: Spawns Edge or Chrome (`msedge --app=http://127.0.0.1:<PORT>/`).
  - **macOS**: Spawns Chrome or Safari via `open -n -a "Google Chrome" --args --app=...`.
- **Privilege & Permission Safety**:
  - When executed under `sudo`, the gateway automatically forwards `$DISPLAY` & `$XAUTHORITY` and drops browser subprocess privileges to `$SUDO_USER`.
  - Infobars and warning banners are suppressed (`--disable-infobars --no-default-browser-check`).
- **Dynamic Port & WS Resolution**:
  - HTML runtime injection dynamically passes `window.__SGRN_WS_PORT__` to the embedded Svelte SPA so standalone direct access connects directly to the gateway's WebSocket port without requiring Nginx.

---

## 5. End-to-End Test Harness & Workflow Commands

### 1. Build Project Binaries & Stage Libraries
```bash
cmake --build .build/linux-static-release/ -j12 --target install
```

### 2. Run Interactive Simulation Demo with Embedded GUI Window
```bash
./demo.py
```

### 3. Run End-to-End Twin Prediction Pipeline Test with Side-by-Side GUI Windows
```bash
./demo_datapipeline.py --gui
```

---

## 6. Python AutoML Module (`sgrn.ml`) Usage

```python
from sgrn.ml import DatasetReader, AutoMLTrainer

# 1. Read extracted dataset & manifest
reader = DatasetReader("manifest.json", "dataset.csv")
df = reader.load_pandas()  # Automatic NaN imputation & categorical factor encoding

# 2. Train candidate models and select champion
trainer = AutoMLTrainer("manifest.json", "dataset.csv")
summary = trainer.train_and_select_best(
    target_feature="TankSkid.tank_level",
    task="regression",
    output_model_prefix="scratch/model"
)

print(f"Champion Model: {summary['champion_model']}")
print(f"Validation R2:  {summary['metric_score']:.4f}")
```
