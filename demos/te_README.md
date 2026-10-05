# TE Rieth experiment (simulated data only)

End-to-end anomaly detection inside SGRN. Simulated TE data, not a live
plant. No real-plant validation is claimed.

Dataset: Rieth et al. 2017, Harvard Dataverse `doi:10.7910/DVN/6C3JR1`.
4 RData files. Columns `faultNumber` (0 normal, 1-20), `simulationRun`
(1-500), `sample` (3-min period), `xmeas_1..41`, `xmv_1..11`. Train runs
500 samples (25 h), test runs 960 samples (48 h). Faults from sample 21
(train) / 161 (test). Load with `pyreadr`, one file at a time.

Schema: `sgrn/lib/gateway/simulations/te_rieth/schema.scl` uses descriptive
twin paths (`TE.reactor_pressure`, …; Rieth column noted per field) with
`#UNIT` (how it is measured) + `#DIMENSION` (what is measured:
flow/pressure/level/temperature/power/concentration/position, declared in
file-top `#DIMENSIONS(...)`; undeclared values fail parse). Dimensions show
in the dashboard next to the unit, in `GET /registry`, and in
`sgrn_dataset` manifests. Renaming twin paths invalidates old archives —
reconvert after any rename.

Other directives used here: `#DESC` (dashboard tooltip), `#LABEL`
(`TELabels`, flagged `is_label` in manifests, never a model feature),
`#PRECISION` (dashboard decimals), `#NOMINAL` (Downs base case, residual
reference). Also available: `#TRANSIENT` (out of JSONL WAL + datasets),
`#READ_ONLY` (semantic POST writes denied), `#ALARM(lo, hi)` (surfaced in
registry/dashboard, not evaluated). Full table in `sgrn/lib/scl/README.md`.

## Run in order

```bash
micromamba activate SGRN
pip install torch pyreadr  # new deps (zstandard/scikit-learn/matplotlib already present)
python3 demos/te_to_jsonl.py --rdata /data/TEP_FaultFree_Training.RData --split train --runs 1 2 3 4 5 6 --out-dir scratch/te
python3 demos/te_to_jsonl.py --rdata /data/TEP_Faulty_Training.RData --split train --faults 1 --runs 1 2 --out-dir scratch/te
python3 demos/te_to_jsonl.py --rdata /data/TEP_FaultFree_Testing.RData --split test --runs 1 2 3 4 5 6 --out-dir scratch/te
python3 demos/te_to_jsonl.py --rdata /data/TEP_Faulty_Testing.RData --split test --faults 1 --runs 1 2 --out-dir scratch/te
python3 demos/te_train.py --indir scratch/te --outdir scratch/te_model --alpha 0.01
python3 demos/te_experiment.py --indir scratch/te --model scratch/te_model --out scratch/te/results.csv --plot scratch/te/example.png --speed 1800 --faults 0 1 --max-runs 2
```

Rebuild C++ after the replay/HTTP changes, then rebuild web headers:

```bash
cmake --preset linux-static-release
cmake --build .build/linux-static-release --target install-binaries
```

Dashboard: open the replay gateway URL; `ReplayControl` shows only on
replay gateways (`/replay/*` 404 on normal gateways). `/` serves the
replay-only dashboard (process image + pacing, no docs bundle) on replay
gateways and the full gateway dashboard otherwise; both stay addressable
as `/replay.html` and `/index.html`.

## Limitations

- Simulated data only, not a live plant.
- Conformal exchangeability does not strictly hold for time series;
  coverage is approximate.
- Detection only, no diagnosis (fault type is never classified).
- Faults 3, 9, 15 are very hard; reported as-is, no tuning around them.
- Binary archives are not generated (known WAL frame-alignment issue;
  jsonl->binary encoder not implemented).
