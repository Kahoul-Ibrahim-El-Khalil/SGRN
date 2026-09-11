# SGRN Gateway

The **SGRN Gateway** is the core network daemon of the SGRN suite. It is a high-performance, low-latency telemetry hub designed for passive, robust ingestion of industrial data and real-time distribution across OT and IT protocols.

---

## Core Architecture: The Digital Twin

The fundamental principle of the SGRN Gateway is the **Digital Twin Memory Architecture**.

Instead of allowing incoming network clients (like a WebSocket dashboard or an OPC-UA client) to query the physical PLC directly, the gateway acts as a protective buffer. It pulls the data from the PLC into a locally maintained, C++ memory space (the Twin).

- **Zero Network Spam:** The PLC is only polled by a single background thread per Data Block, regardless of how many IT clients are connected to the Gateway.
- **Decoupled Reader/Writer Paths:**
  - **Writes to the Twin (from PLC)**: A dedicated polling thread reads from the PLC and updates the local C++ structures. This uses mutexes or lock-free atomics (depending on configuration) to ensure no torn reads.
  - **Reads from the Twin (to Clients)**: When an OPC-UA client reads a value, it simply queries the local memory inside the gateway. This guarantees sub-millisecond response times and protects the PLC from high-traffic spikes.

---

## Multi-Protocol Projection

The SGRN Gateway does not treat protocols in silos. Instead, it projects the single Digital Twin state across multiple endpoints concurrently:

1.  **Modbus TCP:** Exposes designated Data Blocks as Modbus Holding Registers or Coils (via the `#MODBUS_*` SCL directives).
2.  **OPC-UA:** Serves the entire schema hierarchically, mapping UDTs to native OPC-UA ExtensionObjects and structs.
3.  **HTTP/REST:** Provides instant JSON snapshots of the Twin memory.
4.  **WebSockets:** Pushes JSON patch updates to subscribers with minimal overhead.

All protocol adapters bind directly to the internal `PlcSchemaStore` and `Registry` memory mappings, ensuring that a change in the PLC is instantly visible across all protocols.

---

## The Policy Engine

Industrial security requires more than just network firewalls. The SGRN Gateway features a hot-reloadable **AST-based Policy Engine** configured via AngelScript (`.as`) policy files.

- **Granular Access Control:** Rules can be defined down to the specific Data Block (e.g., "Deny write access to DB3 for clients outside 192.168.1.0/24").
- **Protocol Contexts:** Rules can differentiate based on the protocol (e.g., "Allow reads via OPC-UA, but deny reads via HTTP").
- **Lock-Free Evaluation:** The policy engine is designed to evaluate rules on the hot path without blocking network I/O threads, allowing thousands of evaluations per second.

---

## Telemetry Buffering & Persistence

To survive network partitions when forwarding data to upstream cloud servers or local historians (like the SGRN Datastore), the gateway implements an **Anchor-Based Persistence Model**:

- **Batched JSON Lines:** Telemetry changes are batched in memory and asynchronously flushed to disk (typically `/tmp/sgrn-gateway-state`) using JSONL.
- **Anchoring:** Periodically, the gateway writes an "Anchor" frame—a complete snapshot of the Twin's state. Following the anchor, it only writes "Delta" frames (values that changed). This significantly reduces disk I/O and makes recovery extremely fast.
- **Zstandard Compression:** Delta streams can be aggressively compressed using `zstd` before hitting the disk.
