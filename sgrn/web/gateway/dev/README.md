# `sgrn/web/gateway/dev/`

Dev-only config and schema owned by the **gateway dashboard** dev environment.  
These files are used exclusively by `bun run dev` and are never shipped to production.

---

## What's here

| File           | Purpose                                                             |
| -------------- | ------------------------------------------------------------------- |
| `gateway.json` | Gateway configuration for the headless dev instance (port 8001)     |
| `schema.scl`   | SCL tag schema with three DBs covering all major UI component types |
| `README.md`    | This file                                                           |

---

## `schema.scl` — Dev tag schema

The schema contains three data blocks designed to exercise every gateway dashboard UI component:

| DB   | Name       | What it tests                                                                                               |
| ---- | ---------- | ----------------------------------------------------------------------------------------------------------- |
| DB 1 | `DevPlant` | Nested UDTs (`MotorState`, `ValveState`, `AlarmStatus`), scalar reals/ints, booleans, setpoints, mode codes |
| DB 2 | `Energy`   | Flat energy metrics — power, voltage, current, frequency, cumulative `LReal`                                |
| DB 3 | `Sensors`  | **Array types** — `Array[0..N] of Real/Bool` for testing array UI components                                |

### Adding tags

Edit `schema.scl` freely — no recompile required. The gateway reads the schema at startup, so just restart `bun run dev` to pick up changes.

```scl
// Example: add a new tag to DevPlant
DATA_BLOCK "DevPlant" DB 1
#MODBUS_HOLDING
VAR
    // ... existing tags ...
    my_new_tag : Real #UNIT("°C");
END_VAR
END_DATA_BLOCK
```

---

## `gateway.json` — Dev gateway config

Key settings:

| Setting               | Value                                                 |
| --------------------- | ----------------------------------------------------- |
| HTTP + WebSocket port | `8001`                                                |
| Security policy       | `permissive` (no auth required for dev)               |
| State dir             | `/tmp/sgrn-gateway-dev-state`                         |
| Persistence           | enabled, binary format, `changes_with_timestamp` mode |

Edit `gateway.json` to enable/disable northbound features (e.g. add TLS, change port, tweak persistence). Restart `bun run dev` after any change.

---

## `bun run dev` — automatic startup

Running `bun run dev` from `sgrn/web/gateway/` will:

1. Validate that the `gateway` binary exists in the build directory.
2. Start `gateway --headless` using `dev/gateway.json` and `dev/schema.scl`.
3. Start the Vite HMR server on `http://localhost:5173`, proxying API/WS to `:8001`.
4. Kill both processes cleanly on Ctrl-C.

---

## Switching to a shared simulation

Override the config and schema env vars to use any simulation from `sgrn/lib/gateway/simulations/`:

```bash
SGRN_GW_CONFIG=sgrn/lib/gateway/simulations/pump/gateway.json \
SGRN_SCHEMA=sgrn/lib/gateway/simulations/pump/schema.scl \
bun run dev
```

The dev defaults (`dev/gateway.json` and `dev/schema.scl`) are used when neither env var is set.
