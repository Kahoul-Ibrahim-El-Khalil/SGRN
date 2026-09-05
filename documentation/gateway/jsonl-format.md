# JSONL WAL Format (`.jsonl.zst`)

> **Legacy-compatible format.** Still the default (`persistence.format`).
> `RecoveryEngine` restores both formats, newest schema-matching file wins.
> New archival features target the canonical binary WAL (`binary-format.md`)
> first; this document describes the JSONL contract those tools must keep
> reading.

Line-oriented write-ahead log written by `PersistenceService` when
`persistence.format` is `"jsonl"` (the default). Each archive is a single
Zstd frame containing `\n`-delimited JSON objects. This is the only archive
format `RecoveryEngine` reads at boot. See `binary-format.md` for the
high-density sibling and `persistence.md` for service configuration.

## Line types

A healthy archive reads top to bottom as:

| # | `type`       | Written by | Content |
|---|--------------|------------|---------|
| 1 | `schema`     | `openNewArchive()` | `{"schema": <PlcSchemaStore JSON> \| null}` (null skips recovery validation) |
| 2 | `dictionary` | `openNewArchive()` | `{"leaves": [{"id": <uint>, "path": "DbName.field"}]}` — the LeafDictionary |
| 3 | `manifest`   | `openNewArchive()` | `{"start_time": <iso8601>, "mode", "namespaces"}` |
| 4..N-1 | `anchor` / `delta` | see below | state records with `ts` (ms) |
| N | `footer`     | `finalizeArchive()` | `{"last_anchor_line": <1-based line index>, "record_count": N}` |

`RecoveryEngine` streams the file twice: pass 1 learns `last_anchor_line`
from the footer (falling back to a full scan when the footer is missing, e.g.
after a crash); pass 2 decodes the anchor as base state and applies later
deltas. Files are named `<start>-<end>.jsonl.zst` under
`state/unsynced/<date>/` (provisional `*.tmp` suffix until finalize renames).

## Payload shapes: IDs, not paths

Since the LeafDictionary optimization, anchor `data` and delta `changes` are
**flat, numeric-ID-keyed objects** — never the nested `{"Db": {"field": v}}`
shape from older docs:

```json
{"type": "anchor", "ts": 1788616968350, "data": {"0": 0.0, "1": 0.0, "22": 1.0}}
{"type": "delta",  "ts": 1788616968360, "changes": {"0": 120.5}}
```

Consumers MUST resolve IDs through the file-local `dictionary` line
(`id` → `path`) before use. `sgrn_dataset`, `sgrn_replay` (JSONL branch),
`RecoveryEngine`, and `tools/wal_dump` all do this; a consumer that stores
keys verbatim will silently mismatch every lookup (`current_state_["42"]`
never equals feature `"TankSkid.tank_level"`).

Two legacy shapes still occur in old files and are still accepted:

- nested anchor: `{"data": {"DbName": {"field": v}}}` (pre-dictionary),
- flat record: `{"timestamp": 123, "db": "TankSkid", "path": "tank_level", "val": ...}`.

Detection rule used by the readers: if any member value of `data` is an
object, the line is legacy-nested; otherwise keys are IDs (all-digit,
resolvable) or already-dotted paths (used verbatim).

## How records are produced

- **Anchor**: every `FullSnapshot` broker event calls `ingestFullTree()`,
  which flattens the tree to ID-keyed form (`flattenNestedTreeFiltered`) and
  writes one `anchor` line. `GatewayApplication::feedInitialAnchor()` emits
  the startup anchor.
- **Delta**: `DeltaSnapshot` events are parsed (nested, flat-numeric, and
  flat-dotted payloads all accepted), namespace-filtered, and merged into an
  atomic window (`atomic_window_ms`, preserving PLC-scan-cycle atomicity).
  `commitMergeBuffer()` emits one ID-keyed `delta` line per window.
- **LeafUpdate broker events carry typed payloads with no JSON** and are
  intentionally ignored by the JSONL path — the `DeltaSnapshot` from the same
  dirty batch carries the JSON. (Dereferencing their null `json_value` here
  used to segfault the gateway; there is an explicit guard.)

## Tooling

| Tool | Direction |
|------|-----------|
| `sgrn_dataset` (default mode) | `.jsonl.zst` → CSV + manifest (dictionary-resolved, state carried forward per timestamp) |
| `sgrn_replay` | `.jsonl.zst` → live `PlcMemory`: leaves resolve via the dictionary line to schema fields, encode with `encodeFieldAt`, and commit per WAL line as one batched `writeDbMemory` (+ `writeBit` for packed single-bit bools) |
| `tools/wal_dump` | human-readable dump (`expandRecordKeys` for ID→path) |
| `RecoveryEngine` | boot restore from newest schema-matching archive |

## Inspecting a file

```bash
zstd -d state/unsynced/<date>/<start>-<end>.jsonl.zst -o - | head -5
zstd -d state/unsynced/<date>/<start>-<end>.jsonl.zst -o - | grep -c '"type":"delta"'
```

## Merging archives

`sgrn_dataset --merge OUT.jsonl.zst -i <dir>` concatenates archives (JSONL
inputs, or binary inputs which transcode automatically) into one file:
first file's schema/manifest win (schema drift warns), changed `dictionary`
lines stay inline so IDs keep resolving, every footer is dropped and one
recomputed footer (`last_anchor_line`, `record_count`) closes the file.
Inputs should be non-overlapping finalized archives, processed in filename
order. Merging *into* `.bin.zst` accepts binary inputs only.

Inputs may mix directories and explicit files, in any order and repetition:
`-i` is repeatable and positional paths work too; directories expand in
sorted filename order while explicit files keep CLI order:

```bash
sgrn_dataset --merge month.jsonl.zst -i ./week1/ -i ./week2/extra.jsonl.zst
sgrn_dataset --merge month.jsonl.zst part1.jsonl.zst part2.jsonl.zst
```
