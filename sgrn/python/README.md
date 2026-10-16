# sgrn — Python bindings for the SGRN Gateway

Typed HTTP REST client, asyncio WebSocket telemetry client, S7↔NumPy dtype
bridge, and dataclasses for every gateway JSON shape, plus an `ml/` helper
(dataset/trainer for the telemetry AutoML flow).

This is the full reference the package docstring points at.

## Layout

- `gateway.py` — `Gateway`/`GatewayClient`, `GatewayError`
  (`GatewayHTTPError`): all current REST endpoints.
- `telemetry.py` — `GatewayTelemetry` asyncio client (built on
  `websockets`); flatten/filter/buffer pipeline (`TelemetryEngine`) and
  rolling NumPy ring buffer (`NumpyHistory`).
- `dtypes.py` — registry schema → NumPy structured dtype bridge
  (`decodeRecord`, endianness-aware per-field decode).
- `models.py` — dataclasses: `DbField`, `DbSchema`, `UdtSchema`,
  `DataWriteResult`, `ConnectionInfo`, …
- `ml/` — Industrial machine learning and conformal manifold anomaly detection
  (see [sgrn/ml/README.md](file:///home/odahim/Studies/Software/SGRN/sgrn/python/sgrn/ml/README.md)):
  - `ml/device.py` — Hardware accelerator discovery (AMD ROCm/HIP, NVIDIA CUDA, Apple MPS, CPU).
  - `ml/schema.py` — SCL schema parser, semantic dimension taxonomy, and model auto-configuration.
  - `ml/transform.py` — Telemetry scaling, timeline cycle filtering, and sliding window construction.
  - `ml/manifold.py` — PyTorch autoencoder, manifold projection, conformal uncertainty, and dimensional attribution.
  - `ml/pipeline.py` — End-to-end training (`fitManifoldPipeline`) and evaluation (`evaluateManifoldSequence`).
  - `ml/dataset.py` — Binary WAL (`.bin.zst`) and JSONL streaming replay readers.
  - `ml/diagnostics.py` & `ml/trainer.py` — PCA monitoring, feature attribution, and AutoML helpers.

## Install / use

Requires Python ≥ 3.9, `numpy>=1.23`, `websockets>=14.0`
(see `pyproject.toml`; install with `pip install ./sgrn/python`;
ML extras with `pip install ./sgrn/python[ml]` for torch, pandas,
scikit-learn and zstandard).

The repo-root `sgrn/` directory is the C++ tree and must NOT shadow this
package: test scripts put `sgrn/python` on `sys.path` *before* the repo
root. In-tree consumers: `tests/gateway/websocket.py`,
`tests/gateway/rest_api.py`, `tests/scl/dtypes_endianness.py`,
`demos/train_binary_ml.py`.

`sgrn.ml.fitMonitorAndDiagnoser` fits a normal-only PCA monitor, calibrates
its reconstruction threshold on separate data, and fits a fault classifier.
`sgrn.ml.explainSample` ranks feature deviations for a fitted bundle. The
diagnostic functions require scikit-learn when called; install it separately
if it is not already part of your environment.
