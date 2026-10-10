# `s7shell` Synthetic Data Simulation, Replay & Model Training

This directory contains the SCL schema, AngelScript simulation scripts, and
the synthetic-data → ML pipeline for generating datasets compatible with
**`sgrn_replay`** / **`sgrn_dataset`**.

---

## Files

- **`schema.scl`**: Defines SCL User-Defined Types (`UdtBearingMetrics`, `UdtMotorState`) and Data Block `DbTelemetry`.
- **`simulation.as`**: AngelScript simulation script using `SimEngine`, `SimParams`, and `Persistence` to generate compressed `.bin.zst` WAL archives.
- **`synth_gen.as`**: Parameterized twin of `simulation.as` (same physics) rendered
  per `(scenario, seed)` by `train_synthetic.py`. Placeholders `@SEED@`,
  `@FAULT@`, `@DURATION_S@`, `@TIMESTEP_MS@`, `@NOISE@`, `@SCHEMA@`,
  `@OUT_DIR@` are substituted by the driver — do not run it directly.
  NOTE: the runtime must stay on one line as
  `PlcRuntime@ g_rt = PlcRuntime( ... )`; the s7shell pre-scanner only binds
  the `DbTelemetry` accessors to that single-line shape.
- **`train_synthetic.py`**: End-to-end pipeline — generate labeled runs with
  `s7shell`, decode them with `sgrn_dataset`, train + evaluate a
  fault-detection classifier and a bearing-health regressor, save champions.
- **`demo_s7shell_dataset_gen.py`**: Automated Python demo that generates synthetic data and replays the WAL archive end-to-end.
- **`generated/`**: Contains `s7shell_api.as` and schema headers for Neovim / VS Code linter autocompletion.

---

## Quick Start (Synthetic Data → Trained Models)

```bash
python3 sgrn/s7shell/simulations/train_synthetic.py --duration-s 300 --seeds 2
```

This generates `healthy` + `bearing_degradation` runs with `s7shell` +
`schema.scl`, converts them with `sgrn_dataset`, and trains:

- `fault_classifier.joblib` — healthy vs bearing degradation (test F1 ≈ 1.0)
- `health_regressor.joblib` — `Motor1.Bearing.HealthScore` from live sensors

plus `report.json` (metrics, features, champion names) under
`scratch/synth_train/models/`. Useful flags:

```bash
python3 sgrn/s7shell/simulations/train_synthetic.py --help
python3 sgrn/s7shell/simulations/train_synthetic.py --skip-generate  # retrain on existing WALs
```

Notes:

- Rows begin at each archive's first anchor frame, so the line-startup
  transient (first ~50 ticks) is typically not captured; training uses one
  row per PLC tick with a per-scenario time-ordered 80/20 split.
- `s7shell` provides native `sin/cos/sqrt/pow/...` script globals (float +
  double overloads) for physics scripts.

---

## Quick Start (Automated Python Pipeline)

Run the end-to-end Python demo to generate and replay a dataset in one command:

```bash
python3 sgrn/s7shell/simulations/demo_s7shell_dataset_gen.py
```

---

## Step 1: Generate Synthetic Data with `s7shell`

Execute `simulation.as` using `s7shell`:

```bash
s7shell sgrn/s7shell/simulations/simulation.as
```

This generates a compressed binary WAL archive under `./datasets/synthetic/unsynced/run_<timestamp>.bin.zst` containing 30 minutes of fast-forwarded plant telemetry in < 2 seconds.

---

## Step 2: Replay Archive with `sgrn_replay`

Play back the generated archive into a live gateway digital twin or northbound stream using `sgrn_replay`:

```bash
sgrn_replay --archive ./datasets/synthetic/unsynced/run_*.bin.zst --speed 1.0
```

### Options for `sgrn_replay`
- `--speed 1.0`: Play back at 1.0× realtime (use `--speed 2.0` for 2× speed, or `--speed 0` for max speed).
- `--loop`: Continuously loop replay of the archive.
