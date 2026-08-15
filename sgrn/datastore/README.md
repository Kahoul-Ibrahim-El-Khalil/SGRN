# SGRN Datastore

The **SGRN Datastore** is the high-throughput persistence and orchestration engine of the SGRN suite. It is designed to act as a resilient middle-layer between edge nodes (gateways) and enterprise/cloud storage solutions.

---

## 🏛️ Core Architecture

While the Gateway focuses on _real-time state projection_, the Datastore focuses on _historical persistence and querying_. It is built on a distributed, multi-tenant architecture designed to handle high-frequency time-series data without losing a single data point during network outages.

### 1. Ingestion Pipeline

The Datastore exposes a high-performance HTTP REST API that accepts telemetry payloads (JSONL or compressed binary formats) pushed by edge Gateways.

- **Batch Processing:** Edge nodes push changes in batches rather than individually, massively reducing HTTP overhead.
- **Immediate Acknowledgment:** Payloads are immediately acknowledged upon being securely written to a local write-ahead log (WAL) or fast local buffer (like SQLite/RocksDB), decoupling network ingestion from backend storage latency.

### 2. The Anchor-Delta Persistence Model

The Datastore natively understands the Gateway's "Anchor-Delta" persistence strategy:

- **Anchors:** Full snapshots of the digital twin memory space.
- **Deltas:** Sparse payloads representing only the memory offsets that changed since the last anchor.
  The Datastore reconstructs the full state timeline by replaying Deltas over the latest Anchor, allowing it to serve complex temporal queries (e.g., "What was the exact state of DB2 at timestamp X?") with minimal storage footprint.

### 3. Forwarding & Backend Orchestration

The Datastore does not inherently try to replace specialized time-series databases. Instead, it serves as an intelligent orchestrator:

- **TimescaleDB / PostgreSQL:** Data can be structured and pushed to TimescaleDB for complex SQL aggregations.
- **Object Storage (MinIO / S3):** Cold data or raw Anchor-Delta streams can be archived into S3 buckets as immutable JSONL/Parquet files for machine learning pipelines.
- **Resilience:** If the backend database goes down, the Datastore queues the payloads locally. Upon backend recovery, it drains the queue while continuing to accept new edge traffic seamlessly.

---

## Embedded Dashboard

The Datastore comes with its own embedded React dashboard.

- This UI provides administrative control over connected Edge nodes, multi-tenant workspace management, and data retention policies.
- It leverages the Datastore's `/api/history` endpoints to visualize historical trends, serving as a powerful investigative tool for plant managers and data engineers without requiring a third-party BI tool.
