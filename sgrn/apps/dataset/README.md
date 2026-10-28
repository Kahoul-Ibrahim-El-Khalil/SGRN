# sgrn_dataset

Telemetry dataset processor and format converter: turns raw gateway state
archives into ML-ready datasets, or converts/merges archives. Engine is
`DatasetProcessor` (`sgrn::gateway::tools`); this directory holds only the
CLI entry point.

## Role

Four primary modes (merge wins over convert; otherwise single-input process):

- **process (CSV)** (`-i DIR -s SCHEMA.scl`): walks archive files, reconciles
  deltas against carried-forward DB images, emits one CSV row per timestamp
  (`dataset.csv` by default) plus an ML manifest (`manifest.json`).
- **discrete columnar (.sgrnb)** (`-i DIR -s SCHEMA.scl --bin dataset.sgrnb`):
  resamples telemetry into uniform discrete time buckets ($t_k = t_0 + k \cdot \Delta t$)
  using `TimeGridResampler` and writes directly to `.sgrnb` binary columnar
  format with zero CSV intermediate overhead.
- **convert** (`-c FILE` or `-f binary|jsonl|csv`): transcodes between the
  binary (`.bin.zst`) and JSONL (`.jsonl.zst`) archive formats. Note:
  jsonl→binary transcoding is refused (unimplemented).
- **merge** (`--merge OUT -i PATH...`): concatenates archives (files and
  directories, in order) into one, with optional `--format` retarget.
- Also: `--decompress`/`--compress` (Zstd), `--man` (full manual page).

## Discrete Time-Grid Resampling & Columnar Output

Industrial control signals and neural networks have differing temporal semantics:
- Telemetry arrives asynchronously with transport and scheduling jitter.
- Neural networks (MLPs, LSTMs, Transformers) require discrete uniform time steps $\Delta t$.
- Multi-rate PLCs update fast loops (10ms) and slow loops (100ms) on independent cadences.

`sgrn_dataset` natively integrates `TimeGridResampler` and `ColumnarDatasetWriter`
to resolve data sanitization and resampling at extraction time:
- Governed by SCL directives: `#TIMESTAMP_FOR`, `#DATASET_STEP`, `#AGG` (`Mean`, `Max`, `Min`, `Or`, `Last`).
- Zero-order hold gap forward-filling prevents artificial NaNs from breaking training tensors.
- Emits memory-mapped, 64-byte aligned columnar `.sgrnb` files directly consumable by `sgrn_train`.

## Usage

```text
sgrn_dataset -i FILE_OR_DIR [-o OUT_FILE] [-s SCHEMA.scl] [--bin FILE.sgrnb] [--step DURATION] [--no-csv]
```

```bash
# Standard legacy CSV dataset extraction
sgrn_dataset -i ./state -s schema.scl -o dataset.csv -m manifest.json

# Zero-copy discrete columnar dataset extraction (10ms uniform grid, no CSV overhead)
sgrn_dataset -i ./state -s schema.scl --bin dataset.sgrnb --step 10ms --no-csv

# Override primary clock tag
sgrn_dataset -i ./state -s schema.scl --bin dataset.sgrnb --step 100ms --time-tag DB1.Timestamp

# Transcode and merge archives
sgrn_dataset -c run.bin.zst -f jsonl
sgrn_dataset --merge merged.jsonl.zst -i part1/ -i part2.bin.zst
sgrn_dataset --man   # full manual page
```

## Dependencies

Links `sgrn_ml` (resampler & columnar dataset), `sgrn_gateway_twin`,
`sgrn::scl` (schema store), `sgrn_utils`, `sgrn::common`, zstd, cxxopts,
RapidJSON and fmt. Offline tool: no PLC, server, or network access.
