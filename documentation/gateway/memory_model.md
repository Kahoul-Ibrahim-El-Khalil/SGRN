# Memory model

The gateway keeps the plant **digital twin** in a single pre-allocated **byte arena**. Schema (`PlcSchemaStore`) describes how to interpret each DB; runtime state lives in **`PlcArena`** / **`PlcState`**.

---

## Physical layout: `PlcArena`

- One contiguous buffer holds all DB mirrors.
- Each **`DbEntry`**: DB number, name, byte offset in arena, `size`, dirty flag, JSON cache.
- Registration: `PlcMemory::loadRegistry` after parsing the registry.

There is no dynamic schema growth at runtime — new DBs require restart with an updated registry.

---

## Logical layout: `PlcNode` tree

Each DB has a root `PlcNode`:

- Children mirror `DbField` hierarchy (structs, arrays as nested nodes).
- Each node: `offset`, `bit_index`, `s7_type`, `size`, `is_dynamic`, optional `children`.
- Navigation: dot paths (`Tank.pump_1.flow_rate`) via `findSymbol` / `parseFieldTarget`.

### Extended Types & Dynamic Vectors

The gateway extends the traditional S7 memory model to support high-capacity "Virtual PLC" features:

1. **Extended Strings (`XSTRING`, `XWSTRING`)**:
   - Uses an **8-byte header** (4-byte Max Length, 4-byte Current Length) in Big-Endian.
   - Supports payloads up to 4GB (effectively 16MB for most industrial uses).
   - Incompatible with legacy S7 clients, which will only see the high-order bytes of the 32-bit headers.

2. **Dynamic Vectors (`#DYNAMIC ARRAY`)**:
   - Fields marked as dynamic include a **4-byte length header** (uint32) prepended to the data.
   - The recursive serializer only projects `min(header_count, max_capacity)` elements into JSON/OPC-UA.
   - Allows for variable-sized telemetry without re-allocating the arena.

This tree is built once at `loadRegistry` and is not the same as the old “ArenaTree” name in early docs.

---

## Concurrency & Performance Model

The gateway is built for high-throughput, low-latency industrial execution through deterministic memory management and fine-grained concurrency:

### 1. Deterministic Runtime (Zero GC Pauses)
Implemented in C++ using RAII and pre-allocated memory arenas. Unlike managed runtimes (Java, Go, Node.js), execution never suffers from unpredictable Garbage Collector ("Stop-The-World") pauses, ensuring microsecond-predictable response times for physical machine telemetry.

### 2. Lock-Free Registry Lookups (`SnapshotRegistry`)
DB snapshot lookups on hot data paths avoid global mutex contention entirely. The `SnapshotRegistry` uses a direct-indexed, lock-free array (`std::array<std::atomic<DbSnapshot*>, 65536>`) with atomic load/store operations.

### 3. Segment-Level Mutex Isolation
Concurrency control is scoped per Data Block segment rather than across global memory:

| Mechanism | Scope & Description |
|-----------|---------------------|
| `SnapshotRegistry` | **Lock-free O(1)** DB lookup via atomic slots (`std::atomic<DbSnapshot*>`) |
| `DbSnapshot::state_mutex_` | Protects raw byte buffer state for **one specific DB** |
| `cache_mutex` | Protects `cached_full_json` string cache |
| `PlcMemory::dirty_cv_mutex` | Coordinates dirty state notification signals |

Operations targeting different Data Blocks (e.g., S7 poller updating DB1 while WebSocket/OPC-UA threads read DB5) execute in parallel with **zero cross-DB lock contention**.

### 4. DB-Level Snapshot Atomicity (`DoubleBuffer`)
Within each Data Block segment (`DbSnapshot`), byte buffers are managed via a `DoubleBuffer` pattern consisting of a read-only **front buffer** and write-only **back buffer**:
- **Torn-Read Prevention**: Ingested raw bytes are written to the back buffer and committed atomically using `DoubleBuffer::swap()` with release-acquire memory semantics (`std::memory_order_release`).
- **Atomicity Scope**: Readers reading from the front buffer always observe a **100% complete snapshot** of the Data Block (either the full previous DB state or the full new DB state). Readers never observe torn reads or partially-written primitive fields (e.g. 32-bit floats, 64-bit timestamps, or multi-field structs).

---

## Write path

1. Southbound (`S7ProtocolAdapter`, HTTP `PUT` raw) calls **`PlcMemory::writeMemory`**.
2. **`memcmp`** against current bytes — if equal, return (no dirty, no telemetry).
3. Otherwise `memcpy`, **`markDirty()`**, **`signalDirty()`** → **`processDirty()`** → broker (see [arch.md](arch.md#data-flow)).

---

## Read path and JSON

Northbound protocols do not read raw bytes directly for API responses; they use JSON produced from the tree or cache:

| Request | Typical path |
|---------|----------------|
| Full DB, clean | `cached_full_json` |
| Full DB, dirty | Serialize DB → update cache |
| Subtree | Full DB JSON + `extractSubtree`, or `getSubtreeJson` |

Details: [north_facades.md](north_facades.md#architectural-overview).

---

## Endianness

- Default: **big-endian** (S7).
- Override per DB/UDT/field: `#LITTLE_ENDIAN` / `#BIG_ENDIAN` in SCL, or `"endianness"` in JSON.
- Codec layer (`siemens::`) applies endian when encoding/decoding fields to JSON.

---

## Type system

Binary layout follows Siemens rules (bool packing, alignment). JSON mapping uses universal types (`Bool`, `Float`, `Int`, `String`, `DateTime`, …) on `PlcNode` for northbound APIs.

Supported primitives and UDTs: see [SclSchema.md](SclSchema.md).

---

## Immutability

| What | Mutable at runtime? |
|------|---------------------|
| Schema (offsets, types) | **No** — restart required |
| Arena bytes | **Yes** — via southbound writes |
| JSON cache | **Yes** — refreshed on dirty |

---

## See also

- [arch.md](arch.md) — topology, boot sequence, data flow
- [SclSchema.md](SclSchema.md) — SCL dialect, `sclc`, registry format
- [north_facades.md](north_facades.md) — how the twin is exposed northbound
- [rest.md](rest.md) — HTTP endpoint reference
