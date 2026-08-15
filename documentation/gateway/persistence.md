## Persistence & Historian

The **PersistenceService** is SGRN's independent local historian. It archives PLC telemetry to Zstd-compressed JSON files on disk, regardless of whether a cloud backend is configured.

The **DatastoreBridge** is then a simple uploader — it reads the list of locally-archived files from the `pending_batches` SQLite table and pushes them to the cloud when connectivity is available.

```
PLC Write
   │
   ▼
S7ProtocolAdapter
   │
   ▼
PlcCommandProcessor  ──►  TelemetryBroker
                                │
                ┌───────────────┼───────────────┐
                ▼               ▼               ▼
         WebSocket         DatastoreBridge  PersistenceService
         (live UI)         (cloud upload) (local historian)
                                               │
                                 state/unsynced/<date>/<time>.json.zst
                                               │
                                         GatewayDatabase
                                      (pending_batches table)
                                               │
                                         DatastoreBridge
                                         (uploads when online)
```

---

### Two Layers of Persistence

SGRN has **two distinct persistence mechanisms** that serve different purposes and should not be confused:

| Layer | Component | What it saves | Where | Purpose |
|-------|-----------|--------------|-------|---------|
| **Twin State** | `TreePersistenceStore` | Last known value of every PLC field | `state/twin_state.json` | Warm restart — restores the digital twin instantly without waiting for a PLC cycle |
| **Historian** | `PersistenceService` | Time-series change archive as `.json.zst` files | `state/unsynced/<date>/` | Long-term storage, offline buffering, cloud upload queue |

The twin state file is always active. The historian is opt-in via the `persistence` block in `gateway.json`.

---

### Configuration

Add a `persistence` block to your `gateway.json`:

```json
{
  "persistence": {
    "enabled": true,
    "mode": "changes_with_timestamp",
    "namespaces": ["DB10", "DB11.hot_leg_temp"],
    "atomic_window_ms": 10,
    "batch_size": 1000,
    "batch_interval_s": 300,
    "anchor_interval_s": 86400,
    "anchor_change_count": 10000,
    "zstd_level": 5,
    "anchor_zstd_level": 12
  }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `false` | Master on/off switch |
| `mode` | string | `"changes_with_timestamp"` | Archive mode (see below) |
| `namespaces` | array | `[]` | Path prefixes to filter. Empty = all fields |
| `atomic_window_ms` | number | `10` | Merge window (ms) for PLC scan-cycle atomicity. `0` = disabled |
| `batch_size` | number | `1000` | Number of merged records that trigger a disk flush |
| `batch_interval_s` | number | `300` | Seconds between forced flushes (even if batch is not full) |
| `anchor_interval_s` | number | `86400` | Re-issue anchor after N seconds (`full_tree_with_anchor` only, `0` = disabled) |
| `anchor_change_count` | number | `10000` | Re-issue anchor after N accumulated changes (`full_tree_with_anchor` only, `0` = disabled) |
| `zstd_level` | number | `5` | Zstd compression level for delta batches (1–22) |
| `anchor_zstd_level` | number | `12` | Zstd compression level for anchor snapshots |

---

### Archive Modes

#### `changes_with_timestamp` _(recommended)_

Only changed fields are archived. This is the most storage-efficient mode and the default.

Each flush produces a `DELTA_BATCH` envelope containing a list of atomic change records:

```json
{
  "type": "DELTA_BATCH",
  "size": 3,
  "data": [
    {
      "timestamp": "2025-07-12T22:15:00.010Z",
      "changes": {
        "DB10.thermal_power_mw": 1340.5,
        "DB10.thermal_power_pct": 39.4,
        "DB10.neutron_flux": 3.8e13
      }
    },
    {
      "timestamp": "2025-07-12T22:15:00.110Z",
      "changes": { "DB11.hot_leg_temp_c": 325.1 }
    },
    {
      "timestamp": "2025-07-12T22:15:00.210Z",
      "changes": { "DB10.thermal_power_mw": 1341.2 }
    }
  ]
}
```

**Limitation:** To reconstruct the state at an arbitrary point in time you must replay all delta batches from the beginning (or from a known baseline). Use `full_tree_with_anchor` if you need random access.

---

#### `full_tree_with_anchor`

Combines the storage efficiency of delta archiving with the ability to reconstruct state at any point without replaying the full history.

**How it works:**

1. On startup, a full **anchor snapshot** is written immediately.
2. Between anchors, only changed fields are stored as delta batches (same format as `changes_with_timestamp`).
3. A new anchor is re-issued when either trigger fires — **whichever comes first**:
   - **Time-based:** `anchor_interval_s` seconds have elapsed since the last anchor. Set to `0` to disable.
   - **Count-based:** `anchor_change_count` field-change records have accumulated since the last anchor. Set to `0` to disable.

**Anchor file format:**
```json
{
  "type": "ANCHOR",
  "timestamp": "2025-07-12T00:00:00.000Z",
  "data": {
    "DB10": { "thermal_power_mw": 1340.5, "neutron_flux": 3.8e13 },
    "DB11": { "hot_leg_temp_c": 325.1 }
  }
}
```

**Reconstruction algorithm:**
1. Find the most recent anchor file whose timestamp is **≤** the target time.
2. Apply all delta batches in chronological order up to the target time.
3. The result is the complete plant state at the target instant.

**Configuration example** (anchor every hour or every 5000 changes):
```json
{
  "persistence": {
    "mode": "full_tree_with_anchor",
    "anchor_interval_s": 3600,
    "anchor_change_count": 5000
  }
}
```

---

#### `full_tree`

Every flush writes the entire PLC snapshot. No delta reconstruction needed, but at maximum storage cost.

```json
{
  "type": "FULL_TREE",
  "timestamp": "2025-07-12T22:15:00.000Z",
  "data": {
    "DB10": { "thermal_power_mw": 1340.5, "neutron_flux": 3.8e13 },
    "DB11": { "hot_leg_temp_c": 325.1 }
  }
}
```

Use this mode when downstream consumers cannot handle delta reconstruction, or when storage is not a concern.

---

### Atomic Merge Window

A single PLC scan cycle may update dozens of fields almost simultaneously. Each field arrives as a separate `DeltaSnapshot` event fired within ~1ms of the others. Without merging, the archive would contain thousands of single-field records, making it difficult to reason about the complete plant state at any given moment.

<div class="callout">

**The `atomic_window_ms` parameter** defines a merge window. All `DeltaSnapshot` events that arrive within this window of the first event in a group are **merged into a single atomic JSON record** before being appended to the batch. The archive then contains **one record per PLC scan cycle** rather than one record per field.

</div>

**Example — without merging (`atomic_window_ms: 0`):**
```json
{ "timestamp": "T+0ms",   "changes": { "DB10.thermal_power_mw": 1340.5 } }
{ "timestamp": "T+0.1ms", "changes": { "DB10.thermal_power_pct": 39.4 } }
{ "timestamp": "T+0.3ms", "changes": { "DB10.neutron_flux": 3.8e13 } }
```

**Example — with merging (`atomic_window_ms: 10`):**
```json
{
  "timestamp": "T+0ms",
  "changes": {
    "DB10.thermal_power_mw": 1340.5,
    "DB10.thermal_power_pct": 39.4,
    "DB10.neutron_flux": 3.8e13
  }
}
```

**Tuning guidance:**
- Set `atomic_window_ms` to slightly above your PLC cycle time. For a 10ms scan cycle, `10–20ms` is a safe value.
- Set to `0` only if you need sub-cycle event granularity (rare).
- If two scan cycles fall within the window, the later cycle's values overwrite the earlier ones in the merged record. Reduce the window if that is unacceptable.

---

### Namespace Filtering

The `namespaces` array archives only specific parts of the PLC memory. Each entry is a **dot-notation path prefix**:

```json
{ "namespaces": ["DB10", "DB11.hot_leg_temp_c", "DB12.status"] }
```

| Prefix | Matches |
|--------|---------|
| `"DB10"` | All fields in DB10 |
| `"DB11.hot_leg_temp_c"` | Only the single field `DB11.hot_leg_temp_c` |
| `"DB12.status"` | Only the single field `DB12.status` |

An empty `namespaces` array (the default) archives **all fields of all DBs**. Filtering is applied before merging, so only matching fields consume merge window slots and batch storage.

---

### Flush Triggers

Two independent triggers control when the in-memory batch is written to disk:

| Trigger | Config key | Condition |
|---------|-----------|-----------|
| **Size** | `batch_size` | Batch reaches N merged records |
| **Time** | `batch_interval_s` | N seconds elapse since the last flush |

Both are evaluated after each incoming event. The first trigger that fires causes an immediate flush.

**Sizing guidance:**
- A batch of 1000 records with typical 3-field changes compresses to roughly 20–60 KB at `zstd_level: 5`.
- Set `batch_interval_s` low enough that data is not lost on an unexpected restart. `60–300` seconds is typical.
- Increase `batch_size` to reduce file count when the PLC is very noisy.

---

### File Layout

```
state/
├── twin_state.json                     ← digital twin warm-restart file
└── unsynced/
│   └── 2025-07-12/
│       ├── 220000-anchor.json.zst      ← ANCHOR file
│       ├── 220000-220500.json.zst      ← DELTA_BATCH (22:00 → 22:05)
│       └── 220501-221000.json.zst      ← DELTA_BATCH (22:05 → 22:10)
└── synced/
    └── 2025-07-12/
        └── 215900-220000.json.zst      ← moved here after upload
```

- Delta batch files are named `<start_time>-<end_time>.json.zst`.
- Anchor files are named `<time>-anchor.json.zst`.
- After a successful `DatastoreBridge` upload, files are moved to `synced/`.

**Inspect any file:**
```bash
zstd -d state/unsynced/2025-07-12/220000-220500.json.zst -o - | jq .
```

---

### Offline Mode

`PersistenceService` works without any backend configured:

```json
{
  "persistence": {
    "enabled": true,
    "mode": "changes_with_timestamp",
    "batch_interval_s": 60
  }
}
```

Files accumulate in `state/unsynced/`. When a backend is added later, `DatastoreBridge` automatically reads `pending_batches` and uploads the backlog in chronological order.

---

### DatastoreBridge Integration

`PersistenceService` and `DatastoreBridge` are completely decoupled. Their integration is through the SQLite `pending_batches` table:

1. **PersistenceService** writes a `.json.zst` file, then inserts a row into `pending_batches` with `status = 'pending'`.
2. **DatastoreBridge** polls `pending_batches` on its upload timer, uploads each `pending` file, and marks it `synced`.
3. Synced rows and files are deleted by the bridge after confirmation.

You can also write a **custom uploader** by reading `pending_batches` directly — no gateway binary changes required.

---

### Storage Estimates

Rough estimates at `zstd_level: 5` for a busy 200-field PLC updating every 100ms:

| Mode | Approx. size / hour |
|------|-------------------|
| `full_tree` | ~30–120 MB/h |
| `full_tree_with_anchor` | ~2–8 MB/h |
| `changes_with_timestamp` | ~0.5–4 MB/h |

Use `changes_with_timestamp` with `atomic_window_ms ≥ poll_interval_ms` for best efficiency. Daily anchors add a one-time ~50–200 KB overhead.

---

### Troubleshooting

**No files written despite `enabled: true`**
- Check that `state_dir` is writable by the gateway process.
- Verify that at least one `DeltaSnapshot` event has been received (check the WebSocket live view or `GET /data/`).
- For testing, set `batch_size: 1` and `batch_interval_s: 5` to force an immediate flush on the very first change.

**Files are written but the `pending_batches` table is empty**
- `GatewayDatabase` was not passed to `PersistenceService::configure()`. Files accumulate locally but `DatastoreBridge` will not upload them automatically. Upload manually or restart with a properly configured `state_dir`.

**Anchor is never written in `full_tree_with_anchor` mode**
- Anchors originate from `FullSnapshot` events. These are produced by the S7 poller (`s7proxy`) on startup or by calling `ingestFullTree()` directly.
- If only delta events arrive, you will get delta batches but no anchor file. Trigger a manual full snapshot via the REST API, or ensure your poller emits a `FullSnapshot` at startup.

**Records contain only one field per timestamp**
- `atomic_window_ms` is `0` or smaller than the PLC scan cycle. Increase it to cover the full cycle duration.

**High pending_tasks_ counter / dropped events**
- Compression runs on the `heavy_pool` background thread pool and is non-blocking.
- If `pending_tasks_` exceeds 200, incoming events are dropped to protect memory. Reduce `batch_size` to flush more frequently, or increase the `heavy_pool` thread count in the gateway source.

**Old files not being cleaned up**
- `PersistenceService` never deletes files — that is the responsibility of `DatastoreBridge` after a confirmed upload. Run the bridge, or manually delete files from `synced/` once you have confirmed they were uploaded.
