## Config Reference

All gateway configuration is read from a single JSON file passed at startup:

```bash
./gateway gateway.json
```

### gateway.json

The configuration is organized into nested blocks. At least one protocol under `listen` must be present and enabled for the gateway to start, and either `schema` or `symbols_dir` must point at the PLC schema.

```json
{
  "schema": "./simulations/nuclear/schema.scl",
  "state_dir": "/tmp/sgrn-gateway-state",
  "reconnect_ms": 5000,
  "security_policy": "strict",
  "security_script": "./configs/security.as",

  "southbound": {
    "s7": {
      "ip": "0.0.0.0",
      "port": 102,
      "max_clients": 12,
      "pdu_size": 960
    }
  },

  "northbound": {
    "http": { "ip": "0.0.0.0", "port": 8000 },
    "websocket": { "ip": "0.0.0.0", "port": 8001 },
    "opcua": { "ip": "0.0.0.0", "port": 4840 }
  },

  "persistence": {
    "enabled": true,
    "mode": "changes_with_timestamp",
    "format": "binary",
    "namespaces": ["DB1", "DB2"],
    "max_events": 500,
    "batch_interval_s": 40,
    "zstd_level": 10,

    "cloud_sync": {
      "enabled": true,
      "url": "https://localhost/datastore",
      "public_token": "1e78dbe3-1f5b-404f-89c1-ae0c07e98c5c",
      "private_token": "FEcPKSxEuJzpst-MYcR8OlQWzFYxPoVDK9Sk3qmGn6A",
      "sync_interval_s": 30
    }
  }
}
```

### Listen Configuration (Adapters)

| Protocol | Description | Optional? |
|----------|-------------|-----------|
| `s7` | Southbound Snap7 listener. If absent, the gateway boots as a northbound-only proxy (requires an external poller or writer). `little_endian` defaults to `true`. | Yes |
| `opcua` | Northbound OPC-UA server | Yes |
| `http` | Northbound REST API (required for the Web UI) | Yes |
| `websocket` | Northbound WebSocket streaming (required for the Web UI) | Yes |
| `modbus` | Northbound Modbus TCP server (register map comes from `#MODBUS_*` SCL directives — see [modbus.md](modbus.md)) | Yes |
| `ethernetip` | Northbound EtherNet/IP adapter (CIP server) | Yes |

> **Note:** S7 is entirely optional. If it is disabled, SGRN interprets twin memory using `little_endian = true` by default, since it assumes a generic (non-Siemens) source. Endianness is otherwise configured per the rules in [memory_model.md](memory_model.md#endianness).

Defaults used when a block is omitted: `http` → `0.0.0.0:8080`, `websocket` → `0.0.0.0:8081`, `opcua` → `0.0.0.0:4840`, `s7` → `0.0.0.0:102`, `modbus` → `0.0.0.0:502`, `ethernetip` → `0.0.0.0:44818`.

#### Adapter Properties

Each protocol in the `listen` block is configured as a nested object supporting standard network binding properties:

| Field | Type | Description |
|-------|------|-------------|
| `ip` | string | The IP address to bind to (e.g. `"0.0.0.0"` or `"127.0.0.1"`). |
| `port` | number | The TCP/UDP port number to bind to. |

The `s7` adapter also supports the following S7-specific properties:

| Field | Type | Description |
|-------|------|-------------|
| `max_clients` | number | Maximum number of simultaneous Snap7 connections. |
| `pdu_size` | number | The negotiated PDU size to announce (default `960`). |
| `little_endian` | boolean | If `true`, overrides the S7 protocol's default big-endian parsing. |

### Global Options

| Field | Type | Description |
|-------|------|-------------|
| `schema` | string | Path to a single `.scl`/`.udt`/`.db`/`.xml`/`.json` schema file (e.g. a `registry.json` produced by `sclc`) |
| `symbols_dir` | string | Alternative to `schema`: path to a directory of `.scl` files |
| `state_dir` | string | Directory for local databases and persistence files (default: `./gateway-state`) |
| `cache_json_north` | boolean | Caches generated JSON responses for faster northbound HTTP performance |
| `reconnect_ms` | number | Reconnect/retry interval for adaptive pollers (default: `5000`) |
| `verbose` | boolean | Verbose logging |
| `debug` | object | `{ "incoming": bool, "tree": bool }` — debug logging for incoming traffic / twin tree |
| `database_rotation_interval_s` | number | History database rotation interval (default: `86400`) |
| `security_policy` | string | `"relaxed"` or `"strict"` (default: `relaxed`) |
| `security_script` | string | Path to the AngelScript policy file (required if `strict`) |

### Persistence

See [persistence.md](persistence.md) for the full reference on the `persistence` block (`enabled`, `mode`, `namespaces`, `atomic_window_ms`, `batch_size`, `batch_interval_s`, `anchor_interval_s`, `anchor_change_count`, `zstd_level`, `anchor_zstd_level`).

### Datastore / Cloud Backend

The `datastore` block configures the cloud uploader (`DatastoreBridge`):

| Field | Type | Description |
|-------|------|-------------|
| `url` | string | Cloud API base URL |
| `public_token` | string | Public (read) token |
| `private_token` | string | Private (write) token |
| `object_name` | string | Object name / facility id (default: `gateway/default`) |
| `sync_interval_s` | number | Upload poll interval in seconds (default: `30`) |
| `telemetry_enabled` | boolean | Whether telemetry upload is enabled (default: `true`) |
| `upload_mode` | string | Upload mode, e.g. `telemetry`, `raw` |
| `vfs_remote_dir` | string | Remote directory for uploaded snapshots (default: `/gateway/snapshots`) |
| `batch_size` | number | Upload batch size |
| `batch_interval_s` | number | Upload batch interval in seconds |
| `snapshot_mode` | string | e.g. `Anchored` |
| `zstd_level` | number | Zstd compression level for uploads (default: `5`) |
| `aggressive_zstd_level` | number | Zstd level for full-tree anchors (default: `12`) |
| `enable_aggressive_compression` | boolean | (default: `true`) |
| `offline_persistence` | boolean | Keep buffering locally while offline |

A legacy `telemetry_buffer` block is also accepted as an alias for the buffer settings (`batch_size`, `batch_interval_s`, `snapshot_mode`, `zstd_level`, `aggressive_zstd_level`, `enable_aggressive_compression`).

### Nodes (DB ACLs)

The `nodes` block configures per-DB properties. Keys are arbitrary (often the string `"1"`, `"2"`, …), but the value must carry a valid `db_number`.

| Field | Type | Description |
|-------|------|-------------|
| `db_number` | number | DB to instantiate in the twin (if not instantiated, access is denied) |
| `allowed_ip` | string | Restrict DB writes to a specific source IP (legacy ACL) |
| `dirty_tracker` | string | Path to a boolean field that is toggled when any value in the DB changes |
