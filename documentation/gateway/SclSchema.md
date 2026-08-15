# SCL schema definition (SGRN dialect)

Implementation note: the schema pipeline has moved from the legacy `s7registry` tool to the **`sclc`** CLI in the `sgrn/scl` module. This page documents the SCL dialect the parser understands, the `sclc` workflow, and how the result is wired into the gateway.

## Parser scope

Implementation: `sgrn/scl/src/schema/DbSymbolsParser.cpp` (see also `PlcSchemaStore`, `SclCompiler` in `sgrn/scl/src/schema/`).

Supported constructs:

- **`DATA_BLOCK "Name"`** with optional `DB <n>` in the export filename
- **`TYPE "Name"`** (UDT) with `END_TYPE`
- **`STRUCT` … `END_STRUCT`**
- Fields: `name : Type;`
- **`ARRAY[lo..hi] OF Type`**
- Primitives and strings (S7 raw binary encoding) — see [memory_model.md](memory_model.md#type-system)

Not supported for runtime init:

- `BEGIN` … initial values in the DB body
- Optimized-access attribute blocks `{ ... }` (the lexer skips them)

## SGRN custom directives (`#`)

| Directive | Scope | Effect |
|-----------|--------|--------|
| **`#BIG_ENDIAN`** | DB or UDT block | Default **big-endian** field encoding in the arena |
| **`#LITTLE_ENDIAN`** | DB or UDT block | Default **little-endian** |
| **`#UNIT "text"`** | After a field declaration | Sets `unit` metadata in JSON only |
| **`#EVENT_TRIGGER`** | DB, UDT or Field | Enable native OPC UA Event emission for the node |
| **`#MODBUS_*`** | DB block | Expose fields as Modbus registers/coils — see [modbus.md](modbus.md) |

Example:

```scl
DATA_BLOCK "SensorData"
#LITTLE_ENDIAN
#EVENT_TRIGGER
STRUCT
    pressure : Real;   // #UNIT "bar"
    status   : "AlarmStruct" #EVENT_TRIGGER;
END_STRUCT;
BEGIN
END_DATA_BLOCK
```

Equivalent in JSON:

```json
"endianness": "little",
"unit": "bar"
```

### Not implemented

The parser header documents `{ S7_Endianness := 'Little' | 'Big' }` — this **brace syntax is not parsed**. Use `#` directives or JSON.

## Compiling schemas with `sclc`

`sclc` is the schema compiler CLI (`sgrn/scl/apps/sclcompiler.cpp`). Commands:

```bash
sclc compile   --parse ./symbols/ -o registry.json   # aggregate .scl/.udt/.db/.xml/.json into a JSON registry
sclc codegen   --parse ./symbols/ -o plc_schema.hpp  # generate s7codec-compatible C++ header
sclc emit-scl  -i registry.json -o ./output/          # round-trip: JSON registry back to clean .scl
sclc emit-dir  --parse ./symbols/ -o ./canonical/     # normalized directory layout (UDT##-name.udt, DB##-name.db, registry.json)
sclc examples  -o ./examples/                         # sample .scl/.udt files
sclc man                                            # SCL syntax reference manual
```

Run `sclc man` for the full dialect reference.

## Wiring the registry into the gateway

Point `gateway.json` at the compiled schema via the **`schema`** key (a single `.scl`/`.udt`/`.db`/`.xml`/`.json` file, e.g. the `registry.json` produced by `sclc compile`) or via **`symbols_dir`** for a directory of source files:

```json
{
  "schema": "./registry.json"
}
```

Note: the gateway 1.0-era `registry` config key and the legacy `s7registry` binary are gone; config keys are `schema` / `symbols_dir`.

## Memory layout

- **`OffsetTracker`** computes byte offsets and bit indices to match S7 packing rules.
- At startup, **`PlcMemory::loadRegistry`** allocates each DB in the arena and builds **`PlcNode`** children from the `DbField` trees.
- Schema is **immutable** until process restart.

Concurrency semantics (per-DB locking, shared reader / exclusive writer) and the immutability table are documented in [memory_model.md](memory_model.md). Older docs referring to path-level locking on an `ArenaTree` should be read as per-DB locking on `PlcArena`.

## See also

- [memory_model.md](memory_model.md)
- [config.md](config.md) — `schema` / `symbols_dir` / `nodes` config keys
- [modbus.md](modbus.md) — `#MODBUS_*` directives
