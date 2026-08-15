## REST / HTTP API

All REST endpoints are served on the HTTP port (default **8000**).

### Data Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/data/` | Full Digital Twin state (nested JSON, semantic) |
| `GET` | `/data/<path>` | Specific DB or field, e.g. `/data/Mixer/speed` |
| `GET` | `/data/<path>/<N>` | Single array element by zero-based index |
| `POST` | `/data/<path>` | JSON write (partial merge, atomic) |
| `POST` | `/data/<path>/<N>` | Scalar write to single array element |
| `PUT` | `/data/<path>` | Full replacement of field with JSON value |

### Memory (Raw Binary) Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/memory/db/<db>/offset/<o>/size/<s>` | Raw binary read, returns `application/octet-stream` |
| `PUT` | `/memory/db/<db>/offset/<o>/size/<s>` | Raw binary write (atomic per DB) |
| `PUT` | `/memory/batch` | Atomic batch write (multiple DBs) |

### Registry & Diagnostics

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/registry` | Full schema (all DBs with fields, UDTs, tags) |
| `GET` | `/registry?headers=true` | DB headers only (fast) |
| `GET` | `/registry?db=<N>` | Single DB schema |
| `GET` | `/registry/modbus` | Modbus virtual register map (built from `#MODBUS_*` SCL directives) |
| `GET` | `/connections` | Active/recent connections |
| `GET` | `/db/history` | Historical database |
| `GET` | `/db/sessions` | Client sessions |
| `GET` | `/db/logs` | System logs |
| `GET` | `/endpoints` | API documentation |
| `GET` | `/api/policy` | Active security policy (JSON) |

### Example: Read a field

```bash
curl http://gateway:8000/data/ReactorCore/temperature
# → 42.75
```

### Example: Write a field

```bash
curl -X POST http://gateway:8000/data/ReactorCore/setpoint \
  -H "Content-Type: application/json" \
  -d '45.0'
```

### Example: Batch binary write

```bash
curl -X PUT http://gateway:8000/memory/batch \
  -H "Content-Type: application/json" \
  -d '[{"db":1,"offset":0,"size":4,"data":"AAABAQ=="}]'
```

### Security Headers

When the security policy is `strict`, HTTP requests may require an `X-SGRN-Key` header or an allowed Origin. Consult `GET /api/policy` for the active rules.
