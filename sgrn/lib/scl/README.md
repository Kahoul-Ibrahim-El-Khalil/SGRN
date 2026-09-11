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
