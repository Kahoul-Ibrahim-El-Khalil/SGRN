# SGRN demos and experiments

Start here when exploring the repository. These commands use the binaries produced by the Linux build and the scenarios under `sgrn/lib/gateway/simulations/`.

## Live gateway and UI

```sh
python3 demos/gateway.py --list
python3 demos/gateway.py simple_skid
```

The launcher starts the selected scenario's gateway, opens its embedded dashboard, then runs its AngelScript soft-PLC simulation. Press Ctrl+C to stop both processes. Run `python3 demos/gateway.py pump` for the pump skid, or pass a number from `--list` instead of a scenario name. Use `--no-gui` to serve the dashboard in a normal browser, or `--no-gui --no-browser` for headless operation. The dashboard is at `http://localhost:8000/` when the scenario config uses the default port.

The launcher lives in [`gateway.py`](gateway.py). It locates binaries in `.dist/`, `.prefix/`, or `.build/` and reports a build hint when they are missing. Some scenarios bind privileged ports such as S7 102 and Modbus 502; grant the gateway process the required permission before launching those scenarios.

## Experiments

- `python3 demos/datapipeline.py --gui` records gateway telemetry, extracts a dataset, trains an AutoML model, and runs a prediction twin.
- `python3 demos/model.py --help` shows modes for the Tennessee Eastman surrogate, fault diagnosis, uncertainty and safety checks, and report generation.
- `python3 demos/gas_processing.py --help` shows modes for the gas-processing plant experiment, including fault injection, prediction, and live twin runs.
- `python3 demos/train_binary_ml.py <archive-or-directory>` interactively trains a model from binary WAL archives and opens a visualization when matplotlib is available.
- [`history_twin.py`](history_twin.py) records a gas plant run, replays the saved history in one gateway, and runs a live predictor against the replay in a second gateway. It is an end-to-end example of S7Shell persistence, binary WAL replay, gateway telemetry, ML prediction, and the dashboard.

Experiment outputs are written beneath `scratch/` so generated datasets, reports, and plots stay out of the source tree.

### What `history_twin.py` does

The script runs these stages:

1. **Record the plant.** It makes a temporary copy of the gas-processing S7Shell simulation, adds the selected fault and a `Persistence` service binding, and runs it against a temporary gateway. S7Shell writes a timestamped, compressed binary WAL archive (`.bin.zst`).
2. **Prepare the predictor.** It loads `scratch/gas_history_twin/bundle.pkl` if present. Otherwise it trains the gas-processing predictor and caches it there. Use `--retrain` to replace the cache; `--runs` and `--seed` affect training when a new bundle is created.
3. **Replay history.** `sgrn_replay` replays the archive into the **Recorded history** gateway. The predictor subscribes to that gateway's live telemetry and builds model inputs from the replayed tags.
4. **Write the prediction.** Each replay update produces a future outlet dew-point estimate. The script writes it over HTTP to the separate **Model predictions** gateway and verifies the first write by reading the value back. That gateway uses the one-field schema in [`history_twin_prediction.scl`](history_twin_prediction.scl); it does not mirror the plant's full process image.
5. **Show both views.** By default, Chromium opens the replay and prediction dashboards side by side. The prediction dashboard displays the predicted outlet dew point.

The history/twin experiment needs the Linux `gateway`, `s7shell`, and `sgrn_replay` binaries, plus the SGRN Python environment with NumPy, pandas, scikit-learn, and the SGRN bindings. It uses high ports by default. Add `--no-browser` for headless runs; `--keep-up` leaves the gateways running after replay. Run `python3 demos/history_twin.py --help` for all options.

By default, each run records 75 seconds and injects a heater flame-out at tick 12. Select `--fault none`, `heater-flameout`, `pressure-surge`, `feed-overload`, `blower-failure`, or `analyzer-bias`; adjust `--fault-tick`, `--duration`, and replay `--speed` as needed. Pass `--archive <file.bin.zst>` to skip recording and replay an existing archive. Run-specific archives, gateway configurations, and process logs are saved under `scratch/gas_history_twin/run_*/`; the model cache is stored at `scratch/gas_history_twin/bundle.pkl`.

The default recording lasts 75 seconds and injects a heater flame-out at second 12. Change the scenario with `--fault` and `--fault-tick`, or shorten the capture with `--duration`. The model is cached in the output directory after its first training run; `--retrain` refreshes it. Press Ctrl+C during replay to stop the gateways and close the two app windows.

## Where other runnable tools live

- PLC and plant scenarios: [`../sgrn/lib/gateway/simulations/`](../sgrn/lib/gateway/simulations/)
- Build, database, deployment, and code-generation helpers: [`../scripts/`](../scripts/), with task entry points in [`../actions/`](../actions/)
- Integration and protocol examples: [`../tests/gateway/`](../tests/gateway/)
- Build and developer setup: [`../documentation/BUILD.md`](../documentation/BUILD.md)
