# sgrn_wal_dump

WAL archive inspector: expands compressed telemetry archives (`.jsonl.zst`)
back into human-readable per-group directories for debugging.

## Role

Reads one archive file (or every archive under a state dir), decompresses
the Zstd line stream, and regroups frames by schema/dictionary drift: each
group gets `data.jsonl` (delta/anchor records with flat numeric ids expanded
back to dotted field paths via the embedded leaf dictionary), plus
`schema.json` and `manifest.json` (footer `record_count`/`last_anchor_line`
merged in). Manifest-only and footer-only lines are carried over, not
decoded. Prints `Done.` on success.

## Usage

```text
sgrn-wal-dump <archive.jsonl.zst> [--out-dir DIR]
sgrn-wal-dump --dir <state_dir>/unsynced [--out-dir DIR]
```

```bash
sgrn-wal-dump /tmp/sgrn-gateway-state/unsynced --out-dir ./dump
```

`--out-dir` defaults to `.`; multi-archive input lands in `group_NNN/`
subdirectories in chronological order. Unparseable lines are skipped;
a missing directory is an error.

## Dependencies

Links `sgrn_gateway_twin` (leaf-dictionary expansion) and `sgrn_utils`
(Zstd line reader), plus RapidJSON/fmt transitively. No server, no network.
