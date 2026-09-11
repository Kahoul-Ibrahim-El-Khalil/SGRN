# sclc

SCL compiler CLI: parses Siemens S7 symbol sources and emits a canonical
JSON registry, plus code generators. Thin entry point over `sgrn_scl`.

## Role

Subcommands (aliases in parentheses; `compile` is default when omitted):

| Command | Effect |
|---|---|
| `compile` (`cmp`, `c`) | Parse SCL/UDT/DB/XML/JSON inputs (or stdin `-`), emit JSON registry |
| `codegen` (`gen`, `header`, `cpp`) | Generate s7codec-compatible C++ header |
| `emit-scl` (`scl`) | Generate clean `.scl` sources from schema |
| `emit-dir` (`dir`, `canonical`) | Normalized layout (`UDT{idx}-{name}.udt`, `DB{idx}-{name}.db`, `registry.json`) |
| `emit-angelscript` (`emit-as`, `as`) | Declaration-only AngelScript headers |
| `examples` | Starter `.scl`/`.udt` files |
| `man` | Full SCL syntax reference manual |

Inputs: `.scl`, `.udt`, `.db`, TIA Portal `.xml` tag tables, `.json`
registries (for merging), `-` for stdin. Global options: `-o/--output`
(file, dir, or `-` for stdout), `-f/--force`, `-v/--verbose`/`--debug`.

## Usage

```bash
sclc ./symbols/ -o registry.json
sclc Motor.scl                          # compiled JSON to stdout
cat Motor.scl | sclc -
sclc gen ./symbols/ -o plc_schema.hpp
sclc as ./symbols/ -o ./generated/
sclc man
```

## Dependencies

Links `sgrn_scl` (compiler), `sgrn::core` and cxxopts. Pure offline
parser: no PLC, server, or network access. See
`documentation/gateway/SclSchema.md` for the language reference.
