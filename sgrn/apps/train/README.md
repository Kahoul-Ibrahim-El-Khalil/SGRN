# sgrn_train

In-tree PyTorch/LibTorch ML model training CLI for industrial Digital Twin models.

`sgrn_train` trains multivariate regression, anomaly detection, and residual models
directly on edge servers. It supports both legacy CSV datasets and zero-copy binary
columnar datasets (`.sgrnb`).

---

## 1. Zero-Copy Columnar Ingestion (`.sgrnb`) vs. CSV

| Feature | Legacy CSV (`.csv`) | Columnar Binary (`.sgrnb`) |
|---|---|---|
| **Memory Ingestion** | Reads text line by line; string splits, `stod` parsing | Memory-mapped (`mmap`) zero heap copies |
| **Ingestion Complexity** | $O(N \cdot D)$ character parsing operations | $O(1)$ pointer assignment to GPU/PyTorch tensors |
| **Precision** | Truncated to ASCII decimal representations | Exact 32-bit float and 64-bit nanosecond epochs |
| **Memory Alignment** | Unaligned heap vectors | 64-byte aligned SIMD/GPU cache-line boundaries |
| **Embedded Metadata** | Requires external `manifest.json` | Self-contained: includes embedded `ModelManifest` JSON |

---

## 2. Usage & Command Line Options

```bash
sgrn_train -d <DATASET> [OPTIONS]
```

### Options Reference:

- `-d, --data <path>`: Path to training dataset file (`.sgrnb` or `.csv`). Required.
- `-t, --target <name>`: Target column name for prediction. Defaults to embedded label in `.sgrnb` or last column in `.csv`.
- `-s, --spec <path>`: Optional path to custom `FeatureSpec` JSON file.
- `-o, --output-dir <dir>`: Output directory for weights (`.pt`) and manifest (`.json`). Defaults to `./models`.
- `-n, --name <name>`: Model identifier name. Defaults to `sgrn_model`.
- `-e, --epochs <N>`: Training epochs. Defaults to `20`.
- `-b, --batch-size <N>`: Mini-batch size. Defaults to `32`.
- `--lr <rate>`: Learning rate. Defaults to `0.001`.
- `--weight-decay <rate>`: L2 regularization penalty. Defaults to `0.0001`.
- `--optimizer <name>`: Optimizer (`adamw`, `adam`, `sgd`). Defaults to `adamw`.
- `--loss <name>`: Loss function (`mse`, `bce`, `l1`). Defaults to `mse`.
- `--hidden <dims>`: Comma-separated hidden layer dimensions (e.g. `128,64,32`). Defaults to `64,32`.
- `--activation <act>`: Activation function (`relu`, `gelu`, `tanh`, `sigmoid`). Defaults to `relu`.
- `--val-split <ratio>`: Validation split ratio (`0.0` to `0.5`). Defaults to `0.2`.
- `--device <dev>`: Target compute device (`auto`, `cpu`, `cuda`). Defaults to `auto`.

---

## 3. Workflow Examples

### 3.1 Training directly from `.sgrnb` (Zero-Copy)
```bash
LD_LIBRARY_PATH=/home/odahim/micromamba/envs/SGRN/lib \
./.build/linux-static-release/sgrn/apps/train/sgrn_train \
    --data dataset.sgrnb \
    --target DB10.TargetThermalPower \
    --epochs 50 \
    --batch-size 64 \
    --lr 0.0005 \
    --hidden 128,64 \
    --activation gelu \
    --output-dir ./models/thermal_model
```

### 3.2 Output Artifacts
Upon completion, `sgrn_train` generates two files in the output directory:
1. `<model_name>.pt`: Serialized PyTorch TorchScript weights.
2. `<model_name>_manifest.json`: Standardized SGRN `ModelManifest` capturing input feature normalization stats (min/max/mean), output targets, training epoch loss, and weights SHA256 checksum.

This manifest and weights file can be directly hot-loaded into `TwinBridge` inside the running gateway process with zero downtime.
