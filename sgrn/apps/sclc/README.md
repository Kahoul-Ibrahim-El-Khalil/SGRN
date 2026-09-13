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
sclc as ./symbols/ -o ./generated/          # schema surface → schema.as
sclc as ./symbols/ -o ./generated/ --include-shell-api   # + s7shell_api.as (native API only)
sclc as plant.scl --include-predefined -o ./generated/   # as.predefined: native API + schema ambient header
sclc man
```

`emit-angelscript` writes declaration-only `schema.as` by default.
`--include-shell-api` additionally writes `s7shell_api.as` (bare native API
surface, matches `s7shell emit-as`). `--include-predefined` instead writes a
single `as.predefined` combining the native API with the schema surface for
IDE language servers; the two flags are mutually exclusive.

## Dependencies

Links `sgrn_scl` (compiler), `sgrn::core` and cxxopts. Pure offline
parser: no PLC, server, or network access. See
`documentation/gateway/SclSchema.md` for the language reference.
