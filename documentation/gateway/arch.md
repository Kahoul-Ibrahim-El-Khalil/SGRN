## Architecture & Data Flow

Mental model, boot sequence, digital twin internals, the registry/schema system, and how a single PLC write propagates to every northbound client.

### Southbound / Northbound Topology

```
┌─────────────────────────────────────────────────────────────┐
│                     SGRN S7 Gateway                         │
│                                                             │
│  SOUTHBOUND (Optional)                                      │
│  Physical PLC (Siemens S7)  ──PUT/GET──►  S7ProtocolAdapter │
│                                          (Snap7 TCP/102)    │
│                                               │             │
│                                    s7RequestCallback        │
│                                               │             │
│                        ┌──────────────────────▼──────────┐  │
│                        │       Digital Twin Layer         │ │
│                        │  PlcState (PlcArena)             │ │
│                        │  Per-DB: front/back buffers,     │ │
│                        │  dirty flags, version counters   │ │
│                        └──────────────┬───────────────────┘ │
│                                       │ Fan-out             │
│                        ┌─────────────▼───────────────────┐  │
│  NORTHBOUND            │   HttpAdapter    WebSocketFacade│  │
│                        │   OpcUaAdapter   ModbusAdapter  │  │
│                        │   EipAdapter     DatastoreBridge│  │
│                        └─────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Boot Sequence

1. `apps/gateway.cpp` reads `gateway.json`
2. Config validates PLC addresses, northbound ports, security policy path
3. `SecurityManager` loads AngelScript policy (if `security_policy: "strict"`)
4. `PlcState` initialises per-DB arenas from the SCL-compiled registry
5. `S7ProtocolAdapter` starts Snap7 server (TCP/102), if configured
6. All configured northbound adapters (HTTP, WS, OPC-UA, etc.) bind their ports
7. WebSocket telemetry loop starts

### Digital Twin Internals

Each Data Block segment manages two buffers — **front** (read-visible) and **back** (write-staging) — via `DoubleBuffer`. On every PLC write:

1. Protocol callback (e.g. Snap7 `PUT`) delivers raw bytes to `writeDbMemory()`
2. Back buffer receives the written bytes under the segment's `DbSnapshot::state_mutex_`
3. Version counter increments; dirty flag is set
4. `DoubleBuffer::swap()` atomically exchanges front/back buffers using release-acquire memory ordering (`std::memory_order_release`) and notifies all northbound adapters

This double-buffer design provides **DB-level snapshot atomicity** (preventing torn reads across multi-byte fields and struct variables) while ensuring wait-free snapshot reads for northbound consumers without global lock contention. DB lookups themselves use a **lock-free** direct-index table ([`SnapshotRegistry`](file:///home/odahim/Studies/Software/SGRN/sgrn/gateway/include/sgrn/gateway/twin/SnapshotRegistry.hpp#L22-L25)). For physical arena layout, extended types, and complete concurrency rules, see [memory_model.md](memory_model.md#concurrency--performance-model).

> **Endianness Note:** Siemens PLCs use Big Endian byte order. When the S7 protocol adapter is used, the Twin naturally expects Big Endian data. If S7 is disabled and the Twin is fed by generic HTTP/northbound writers, SGRN defaults to `little_endian = true` — see [memory_model.md](memory_model.md#endianness) for the full rules and per-DB overrides.

### Schema Registry

The gateway reads SCL (Structured Control Language) definitions compiled at build time into the schema registry. Clients query it at `GET /registry` to get the full field hierarchy: DB number, name, type, byte offset, bit index, and nested UDT definitions. The SCL dialect and directives are documented in [SclSchema.md](SclSchema.md).

## Data Flow

### Write Propagation Chain

A southbound write flows through the adapter, the twin, and out to every northbound consumer:

```
PLC  ─PUT/GET─►  Snap7 Server (TCP/102)
                       │
                 s7RequestCallback()
                       │
              writeDbMemory(db, offset, data)
                       │
              Back buffer receives bytes (atomic)
              Version counter increments
              Dirty flag set
                       │
              PlcCommandProcessor swap task
                       │
            ┌──────────┴──────────────┐
            │  Front ↔ Back swap      │
            │  (per-DB mutex)         │
            └──────────┬──────────────┘
                       │
             notifyNorthbound() fan-out
                       │
     ┌─────────────────┼─────────────────┐
     │                 │                 │
  HTTP REST       WebSocket        OPC-UA/Modbus
  (on next req)   TelemetryBroker  (subscription update)
```

Northbound clients never talk to the PLC directly: every `GET /data`, OPC-UA Read and Modbus poll serves the local twin, so IT polling cannot starve the automation controller.

### WebSocket Delta Snapshots

The `TelemetryBroker` maintains a list of dirty DB snapshots. On each tick it computes a **delta** — only fields that changed since the last flush — and pushes a JSON patch to all WebSocket subscribers:

```json
{
  "ReactorCore": {
    "temperature": 42.75,
    "pressure": 1.013
  },
  "PumpStation": {
    "flow_rate": 12.4
  }
}
```

Clients flatten this into a `db-field` keyed map for fast O(1) cell updates. Subscription control (`subscribe`/`unsubscribe`/`clear_subscriptions`) is documented in [websocket.md](websocket.md).

### HTTP REST (On-Demand)

REST reads always serve the current **front buffer** snapshot. Writes go through the same `writeDbMemory` → swap path, ensuring consistency with PLC-originated writes. Full endpoint reference: [rest.md](rest.md).

### Cloud Backend

If a `datastore` block is configured, `DatastoreBridge` acts as a pure uploader: it polls the `pending_batches` table for files written by `PersistenceService`, uploads them to the cloud, and moves them to `synced/` on success. The upload interval is configurable (default: 5s when online, 15s when reconnecting). `PersistenceService` is the sole writer of `.json.zst` archive files; `DatastoreBridge` never writes files itself. Details, offline mode and storage estimates: [persistence.md](persistence.md).

### See also

- [memory_model.md](memory_model.md) — arena layout, concurrency, immutability
- [SclSchema.md](SclSchema.md) — SCL dialect, directives, `sclc`, registry format
- [rest.md](rest.md) — HTTP endpoint reference
- [persistence.md](persistence.md) — local historian & cloud upload
