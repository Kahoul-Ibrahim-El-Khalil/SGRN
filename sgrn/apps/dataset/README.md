# sgrn_dataset

Telemetry dataset processor and format converter: turns raw gateway state
archives into ML-ready datasets, or converts/merges archives. Engine is
`DatasetProcessor` (`sgrn::gateway::tools`); this directory holds only the
CLI entry point.

## Role

Three modes (merge wins over convert; otherwise single-input process):

- **process** (`-i DIR -s SCHEMA.scl`): walks archive files, reconciles
  deltas against carried-forward DB images, emits one CSV row per timestamp
  (`dataset.csv` by default) plus an ML manifest (`manifest.json`).
- **convert** (`-c FILE` or `-f binary|jsonl|csv`): transcodes between the
  binary (`.bin.zst`) and JSONL (`.jsonl.zst`) archive formats. Note:
  jsonl→binary transcoding is refused (unimplemented).
- **merge** (`--merge OUT -i PATH...`): concatenates archives (files and
  directories, in order) into one, with optional `--format` retarget.
- Also: `--decompress`/`--compress` (Zstd), `--man` (full manual page).

## Usage

```text
sgrn_dataset -i FILE_OR_DIR [-o OUT_FILE] [-f FORMAT] [-s SCHEMA.scl]
```

```bash
sgrn_dataset -i ./state -s schema.scl -o dataset.csv -m manifest.json
sgrn_dataset -c run.bin.zst -f jsonl
sgrn_dataset --merge merged.jsonl.zst -i part1/ -i part2.bin.zst
sgrn_dataset --man   # full manual page
```

## Dependencies

Links `sgrn_gateway_twin`, `sgrn::scl` (schema store), `sgrn_utils`,
`sgrn::common`, zstd, cxxopts, RapidJSON and fmt. Offline tool: no PLC,
server, or network access.
