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
- `websocket.py`, `telemetry.py` — transport clients.
- `ml/` — dataset/trainer helpers plus reusable PCA anomaly monitoring,
  fault diagnosis, and feature-contribution explanations in `diagnostics.py`.

## Install / use

Requires Python ≥ 3.9, `numpy>=1.23`, `websockets>=12.0`
(see `pyproject.toml`; install with `pip install ./sgrn/python`).

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
