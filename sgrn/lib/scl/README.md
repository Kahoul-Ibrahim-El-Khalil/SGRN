# SCL Schema Compiler (`sgrn/scl`)

The **SCL Schema Compiler** is the foundational C++ library that enables SGRN's declarative architecture. It bridges the gap between Siemens automation code and IT software engineering by parsing PLC memory models and constructing a dynamic `PlcSchemaStore`.

---

## The Purpose of SCL Parsing

In legacy SCADA architectures, C++ or C# gateway applications hardcode the byte offsets of PLC memory. If an automation engineer adds a new `Bool` (1 bit) to a PLC struct, it shifts the memory offset of every subsequent variable. This forces the IT team to recalculate offsets, rewrite their structs, and recompile the gateway.

SGRN solves this by decoupling the binary from the PLC structure. The `scl` module parses the native `.scl` or `.awl` source files exported directly from Siemens TIA Portal, dynamically infers the memory layouts, and builds a JSON-serializable representation of the PLC schema.

---

## Compilation Pipeline

The library operates in three distinct phases:

### 1. Tokenization & AST Generation

The `DbSymbolsParser` performs lexical analysis and builds an Abstract Syntax Tree (AST) of the SCL file. It understands:

- Siemens native Data Types (`Real`, `DInt`, `Time`, `String[10]`, etc.)
- `ARRAY` declarations and boundaries.
- User Defined Types (`UDT` or `STRUCT` nesting).
- SGRN-specific inline annotations (e.g., `#MODBUS_HOLDING`, `#UNIT "bar"`, `#BIG_ENDIAN`).

### Semantic annotations: unit vs dimension

- `#UNIT("kPa")` — HOW a value is measured (engineering unit).
- `#DIMENSION("pressure")` — WHAT is measured (physical dimension class).
- `#DIMENSIONS("pressure", "temperature", "flow")` — file-level vocabulary,
  conventionally at the top before the first `DATA_BLOCK`/`TYPE`. When
  present, every `#DIMENSION`   in the file must be a member, otherwise parsing fails (typo guard). When absent, any `#DIMENSION` is accepted.
- Dimensions ride the same paths as units: field JSON (`"dimension"`),
  schema binary codec, `sgrn_dataset` manifest features, `GET /registry`,
  and the dashboard (shown next to the unit).

### Semantic annotations reference

| Directive | Form | Meaning | Consumers |
|---|---|---|---|
| `#UNIT` | `#UNIT("kPa")` | HOW measured (engineering unit) | registry, dashboard, OPC-UA, manifest |
| `#DIMENSION` | `#DIMENSION("pressure")` | WHAT measured (dimension class) | registry, dashboard, manifest |
| `#DIMENSIONS` | `#DIMENSIONS("pressure", …)` top of file | allowed vocabulary; undeclared values fail parse | parser |
| `#DESC` | `#DESC("…")` | human description | dashboard tooltip, registry, manifest |
| `#LABEL` | bare | label/metadata, not a model feature (CSV column kept, flagged `is_label`) | `sgrn_dataset` manifest, trainers |
| `#PRECISION` | `#PRECISION(2)` (0..18) | dashboard display decimals (default 4) | dashboard |
| `#NOMINAL` | `#NOMINAL(2700.0)` | expected operating point (residual reference, not a control target) | models, manifest |
| `#TRANSIENT` | bare | excluded from JSONL WAL + datasets (binary WAL keeps full images for resync) | persistence, `sgrn_dataset` |
| `#READ_ONLY` | bare | semantic `POST /data/*` writes denied regardless of ACLs (raw `/memory/*` stays DB-ACL) | HTTP adapter |
| `#ALARM` | `#ALARM(lo, hi)`, hi > lo | acceptable band, surfaced in registry/dashboard; evaluation is the consumer's job | dashboard, manifest |
| `#RANGE` | `#RANGE(min, max)` | sensor span | registry, dashboard |
| `#ENUM` | `#ENUM(A=1, …)` | named states | registry, dataset (`is_categorical`) |

Inheritance: scalar `TYPE` aliases (`TYPE "P" : Real #UNIT…`) carry
unit/dimension/desc/precision/nominal/alarm/min/max/enum to fields unless
the field overrides them. `#LABEL`/`#TRANSIENT`/`#READ_ONLY` are per-signal
roles and are never inherited.

### 2. Offset Inference & Alignment Mapping

Unlike modern memory-managed languages, Siemens S7 PLCs have strict, proprietary memory alignment rules (e.g., bits are packed into bytes, words align to 2-byte boundaries).
The compiler simulates the TIA Portal memory allocator:

- It tracks the current byte and bit offset.
- It dynamically packs `Bool` values into adjacent bits (0.0 to 0.7).
- It calculates the absolute byte span (`rawTypeSpanBytes`) of complex structs and strings.
- It outputs a resolved `DbField` tree where every node contains its exact `offset` and `bit_index`.

### 3. Serialization (`SchemaSerializer`)

Once the AST is resolved and memory offsets are inferred, the compiler serializes the entire `PlcSchemaStore` into JSON.

- This JSON serves as the universal contract between the SGRN Gateway, the `s7shell`, and Web Dashboards.
- The `SchemaSerializer` allows the Gateway to boot directly from the JSON schema on subsequent runs, avoiding the need to re-parse SCL strings on edge devices.

---

## Schema Registry API

The resulting `PlcSchemaStore` provides constant-time (O(1)) lookups for protocol handlers:

- `getDbByNumber(uint16_t id)`
- `getUdtByName(const std::string& name)`
- Path-based resolution for semantic queries (e.g., routing HTTP `GET /api/db/1/InletSeparation/feed_pressure` down to the specific `DbField` node).
