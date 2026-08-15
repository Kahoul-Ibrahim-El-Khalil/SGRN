# North-Bound Facades

The North-Bound Facades are responsible for abstracting data access and exposing the system's underlying memory model through various protocols. By treating the gateway's memory model as a flat tree supporting `update` and `retrieve` operations—but no insertions—the facades provide a unified interface for accessing nested data structures, including specific fields, nested objects, or array indices.

## Architectural Overview

*   **Dynamic paths:** HTTP and OPC UA paths are derived from **`PlcSchemaStore`** DB names and field trees (e.g. `SensorData.temperature`).
*   **Data serialization:**
    *   **Cached whole DB:** if the DB is clean and `cache_json_north` is enabled, return the cached JSON blob.
    *   **Subtree:** whole-DB JSON + `extractSubtree`, or live subtree serialization for nested OPC UA nodes.
    *   **HTTP fast path:** a `live_cache_` updated from `TelemetryBroker` → `DeltaSnapshot` events.
*   **Protocols:** HTTP/REST, WebSocket (ixwebsocket), OPC UA (open62541).

## HTTP/REST

The HTTP interface exposes the memory model dynamically from the `Registry`. The **canonical endpoint reference is [rest.md](rest.md)** — including the semantic `/data/...` routes, the raw binary `/memory/...` routes, registry and diagnostic endpoints.

Behavioral notes for API consumers:

- `GET /data/` returns the full Digital Twin as nested JSON.
- `POST /data/<path>` is an atomic **merge** write (missing fields are left unchanged);
- `PUT /data/<path>` is a **full replacement** of the field.
- Raw binary reads/writes live under `/memory/` (`/memory/db/{db}/offset/{o}/size/{s}`, `PUT /memory/batch`), not under `/data/`. They use S7 byte semantics and `application/octet-stream` payloads.

## WebSocket

On connect, `WebSocketAdapter` sends the **full plant** JSON (`getDigitalTwinJsonString()`); from then on, `TelemetryBroker` pushes `DeltaSnapshot` JSON to the client. A client can narrow the stream with the `path`-based commands below (a DB-level subscription uses the DB name as the path):

```json
{ "command": "subscribe", "path": "ReactorCore" }
{ "command": "unsubscribe", "path": "ReactorCore/speed" }
{ "command": "clear_subscriptions" }
```

There is no URL-based path form such as `/ws/v1/subscribe/<db>/<path>`. The wire protocol and worker integration are documented in [websocket.md](websocket.md).

## OPC UA

The OPC UA server (`OpcUaAdapter`) maps the memory tree into an information model:

- **Node Access:** paths are mapped to NodeIDs, allowing standard clients to browse the memory tree and perform Read/Write operations.
- **Subscriptions:** leaf fields and **`.Value`** aggregate variables (whole-DB or struct JSON snapshot) support MonitoredItems.
- Parent aggregates refresh when children change (`notifyAggregateAncestors`).

See [opcua.md](opcua.md) for the node layout and security policy integration.

---

## See also

- [rest.md](rest.md) — complete REST / HTTP endpoint table
- [websocket.md](websocket.md) — WebSocket wire protocol
- [opcua.md](opcua.md) — OPC UA node model
- [memory_model.md](memory_model.md) — the memory model these facades project
