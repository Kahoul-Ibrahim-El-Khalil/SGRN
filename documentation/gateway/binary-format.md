# Binary WAL Format (`.bin.zst`)

> **Canonical on-disk format.** New archival features (delta frames, verifiable
> anchors, resync) target this format first. The JSONL WAL
> (`jsonl-format.md`) remains supported — and remains the default.
> `RecoveryEngine` restores both formats, newest schema-matching file wins.

High-density write-ahead log written by `PersistenceService` when
`persistence.format` is `"binary"` (or `"bin.zst"`). Each archive is a single
Zstd frame; decompressed bytes are parsed with native (little-endian on x86)
integer layout. See `jsonl-format.md` for the JSONL sibling format and
`persistence.md` for service configuration.

## Layout overview

```
┌──────────────────────────────────────────────────────────────┐
│ Header: "SGRN" (4B) + version:u16 (2B) + schema_len:u32 (4B) │
│         + schema JSON (schema_len bytes)                     │
├──────────────────────────────────────────────────────────────┤
│ Frame: ts:u64/i64 (8B) + db_num:u16 (2B) + payload_len:u32   │
│        (4B) + payload (payload_len bytes)                    │
│ Frame: ...                                                   │
└──────────────────────────────────────────────────────────────┘
```

There are no line breaks, no per-line JSON, and no footer sentinel: after the
header the file is an uninterrupted run of frames until EOF. A reader stops at
the first truncated frame (`pos + payload_len > size`).

## Header

| Field        | Size | Type   | Notes                                              |
|--------------|------|--------|----------------------------------------------------|
| magic        | 4    | bytes  | ASCII `SGRN`                                       |
| version      | 2    | uint16 | Format version: `1` (full-image frames), `2` (v1 + delta frames), `3` (v2 + anchor frames) |
| schema_len   | 4    | uint32 | Byte length of the schema JSON that follows        |
| schema       | N    | UTF-8  | `PlcSchemaStore::toJson()` (may be `{}` / short)   |

Writers stamp `kBinaryWalVersion` (`2`); readers refuse `ver < 1` or
`ver > kBinaryWalVersion` instead of misparsing.

## Frame types

The 14-byte envelope `(ts:u64)(db_num:u16)(payload_len:u32)` is shared; the
`db_num` selects the kind:

| `db_num` | Kind | Payload |
|----------|------|---------|
| real DB id | **full-image frame** (v1; legacy) | whole DB image, offset 0, `size` bytes |
| `0xFFFE` (`kDeltaFrameDbNum`, v2+) | **delta frame** | `db:u16` (real target) + `(offset:u32, len:u32, bytes…)*` runs against the last image of that DB |
| `0xFFFD` (`kAnchorFrameDbNum`, v3+) | **anchor frame** | `db:u16` (real target) + `crc32:u32` + full DB image |
| `0xFFFF` (`kControlFrameDbNum`) | **control frame** | JSON (dictionary / manifest / anchor / footer) |

Delta runs never overlap and are self-delimiting within `payload_len`; a
delta for a DB with no cached keyframe is skipped, a structurally corrupt one
seeks resync (see below).

## Boot recovery

`RecoveryEngine` restores `.bin.zst` archives (newest schema-matching file
wins, JSONL and binary compete by timestamp): single pass into per-DB
images — full frames replace, CRC-verified anchors replace, deltas patch —
committed to the arena at EOF under one segment lock each, with no dirty
flags and no events. A torn tail keeps whatever parsed; an archive that
yields zero usable images never shadows older archives. Schema mismatch,
unreadable files, and version drift fall through to the next candidate with
an explicit warning.

## Anchor frames: verification and resync

Every full-image emission is an anchor frame: `db + CRC32(IEEE) + image`.
Readers recompute the CRC over the decoded image before trusting a byte —
a mismatch never poisons the image cache. Anchors are emitted for the first
frame of each DB per file, at least every 60 s per DB
(`kBinaryKeyframeIntervalMs`), for large changes, and for every known DB on
each `FullSnapshot` (alongside the JSON anchor control line, which exists
for transcode compatibility).

On any stream corruption (truncated tail, corrupt delta runs, CRC mismatch),
readers scan forward for the next verifiable anchor (`findNextAnchorFrame` —
a CRC hit is ~2^-32 false-positive) and resume there ("corrected"); when no
anchor follows they stop cleanly ("interrupted"). All binary consumers do
this uniformly: `sgrn_dataset`, `sgrn_replay`, `sgrn/ml/dataset.py`,
binary→binary merge, and binary→jsonl transcode.

## Data frames: full-DB snapshots

A data frame payload is a **full raw DB image**: `size` bytes starting at DB
offset 0, byte-identical to the segment's arena memory. This is the invariant
every reader depends on:

- `sgrn_dataset` decodes each field at its schema offset inside the payload
  (`endian_helper::loadValue`, big-endian).
- `sgrn_replay` writes the payload at offset 0 via
  `PlcMemory::writeDbMemory()`.

Because `TelemetryEvent` typed payloads are **per-leaf slices with no offset**,
the writer must NOT store them directly. Instead, on each event it pulls the
full image through a reader callback wired to `PlcMemory` (`FullDbReadFn`,
`GatewayApplication::initInfrastructure()`), and writes a frame only when the
image differs from the last one written for that DB (per-DB dedup,
`last_db_bytes_`). Consecutive identical images are therefore collapsed into
one frame — value-preserving: replaying the archive reproduces the exact same
state trajectory with fewer frames. The dedup cache resets on every
`openNewArchive()` so each file starts with a state-establishing frame.

Timestamps are event milliseconds (`ts = event.timestamp`, wall clock at
capture). Frames from all DBs share one chronological stream.

## Delta policy (writer)

On each change the writer diffs the new image against the last one written
for that DB and emits whichever is smaller:

- **delta frame** when `2 + Σ(8 + run_len)` < 50% of the full image
  (`kBinaryDeltaMaxRatio`), as `(offset,len,bytes)` runs;
- **anchor frame** otherwise, for the first frame of each DB per file, at
  least every keyframe interval per DB, and for every known DB on each
  `FullSnapshot` (alongside the JSON anchor control, kept for transcode
  compatibility). Since v3 the writer never emits a bare full-image frame:
  every data frame self-identifies as delta (`0xFFFE`) or anchor (`0xFFFD`).

The keyframe interval is inferred from configuration: `anchor_interval_s`
when set, else 60 s (`kDefaultBinaryKeyframeIntervalMs`). One anchor cadence
serves both formats; unlike JSONL anchors it never disables entirely, since
binary deltas need restart points for resync. The effective interval lands in
the file's manifest (`keyframe_interval_s`, plus `delta_ratio`); the footer
reports `anchor_count` / `delta_count` next to `last_anchor_line` /
`record_count`.

Net effect on a typical slowly-changing DB: ~6–10× smaller decompressed
stream. On-disk (zstd) gains are modest — one continuous zstd frame already
cross-compresses near-identical full images — but parse/decode work and the
decompress-size ceiling scale with the raw size, so the win lands where
readers pay.

## Control frames (`db_num = 0xFFFF`)

JSON control records (dictionary / manifest / anchor / footer) are wrapped in
the **same frame envelope** with the reserved sentinel DB number
`kControlFrameDbNum = 0xFFFF`
(`include/sgrn/gateway/database/PersistenceService.hpp`). `0xFFFF` can never
be a live DB number (live IDs are small; synthetic arena IDs start at 1000).

| Writer call site                              | Control JSON content                          |
|-----------------------------------------------|-----------------------------------------------|
| `openNewArchive()`                            | `{"type":"dictionary","leaves":[{"id","path"}]}` |
| `openNewArchive()`                            | `{"type":"manifest","start_time","mode","namespaces"}` |
| `writeAnchorLine()` (every `FullSnapshot`)    | `{"type":"anchor","ts","data":{"<id>":val}}` (flat ID-keyed) |
| `finalizeArchive()`                           | `{"type":"footer","last_anchor_line","record_count"}` |
| `writeDeltaLine()` (defensive; unreachable — the binary event path never fills the merge buffer) | delta JSON |

Raw JSON lines must NEVER appear in a binary archive: readers parse every
14-byte header as `ts/db/len`, so unframed text is misdecoded as a garbage
frame and aborts the parse loop immediately. (`RecoveryEngine` only scans
`.jsonl.zst`, so binary archives are not a boot-recovery source.)

Readers (`DatasetProcessor::process`, `GatewayReplayer::processBinaryArchive`,
`sgrn/ml/dataset.py:BinaryDatasetReader`) check
`db_num == kControlFrameDbNum` first and parse-or-skip the JSON payload
instead of decoding it as memory. `0xFFFE` frames patch runs into the
reader's cached image for the embedded DB (replay applies them as one atomic
batched `writeDbMemory`; dataset decodes the row from the patched image).

## Schema drift policy

- **sgrn_replay**: exact-size payload → `writeDbMemory(db, 0, len)`; larger
  payload → truncated to the live segment (prefix fields stay exact, logged);
  smaller payload or unknown DB → skipped and counted. A per-DB
  skipped/truncated summary prints at the end so drift is visible instead of
  silently absorbed into the frame count.
- **sgrn_dataset**: fields are decoded only when the full field width fits
  (`offset + sizeof(type) <= payload_len`); truncated tail fields render as
  empty cells, and a cut-off tail logs `[sgrn_dataset] N trailing bytes
  discarded`.

## Tooling

| Tool | Direction |
|------|-----------|
| `sgrn_dataset` (default mode) | `.bin.zst` → CSV + manifest (control frames skipped) |
| `sgrn_dataset -f jsonl` | `.bin.zst` → `.jsonl.zst` with values preserved: control JSON passes through verbatim; full/anchor frames decode every dictionary-mapped leaf to flat ID-keyed `anchor` lines; delta frames decode changed leaves to `changes` lines (images cached per DB, endian- and bit-aware) |
| `sgrn_dataset -f binary` from JSONL | **not implemented** (`convertFormat` refuses; jsonl→binary transcoding is an open follow-up) |
| `sgrn_replay` | `.bin.zst` → live `PlcMemory` (control frames skipped, pacing by frame `ts`) |
| `sgrn/ml/dataset.py` | `.bin.zst` → NumPy (same control-frame skip) |

## Merging archives

`sgrn_dataset --merge OUT.bin.zst -i <dir>` concatenates binary archives:
first file's header wins (schema drift warns, merge continues), frames copy
verbatim in sorted order, footer control frames are dropped (they describe the
old file, not the merged stream), truncated tails warn per file. Merging into
a binary target refuses JSONL inputs (transcoding unimplemented); merge to
`.jsonl.zst` instead, which transcodes binary inputs automatically.

Like JSONL merging, inputs may mix directories and explicit files (`-i` is
repeatable, positional paths work); directories expand sorted, explicit files
keep CLI order.

## Worked example (Python)

```python
import struct, zstandard
raw = zstandard.ZstdDecompressor().stream_reader(open("a.bin.zst","rb")).read()
assert raw[:4] == b"SGRN"
ver, schema_len = struct.unpack("<HI", raw[4:10])
pos = 10 + schema_len
while pos + 14 <= len(raw):
    ts, db, ln = struct.unpack("<qHi", raw[pos:pos+14]); pos += 14
    if pos + ln > len(raw): break  # truncated tail
    payload = raw[pos:pos+ln]; pos += ln
    if db == 0xFFFF: continue      # control frame (JSON)
    ...decode payload at schema offsets...
```
