## Modbus TCP

The gateway exposes selected PLC fields as Modbus TCP coils and holding registers, enabling integration with legacy HMI and SCADA systems.

### Configuration

The `listen.modbus` block only selects the bind address and port. The register/coil map itself is **not** authored in `gateway.json` — it is derived from the SCL schema (see below):

```json
{
  "listen": {
    "modbus": {
      "ip": "0.0.0.0",
      "port": 502
    }
  }
}
```

### Virtual Register Map (`#MODBUS_*` directives)

Mark fields in your `.scl`/`.udt` schema with a block-level directive to expose them over Modbus:

| Directive | Modbus area |
|-----------|-------------|
| `#MODBUS_HOLDING` | Holding registers (FC 03/06/16) |
| `#MODBUS_INPUT` | Input registers (FC 04) |
| `#MODBUS_COIL` | Coils (FC 01/05/15) |
| `#MODBUS_DISCRETE` | Discrete inputs (FC 02) |

The `ModbusMap` builder assigns discrete addresses automatically at schema load time. If no DB is annotated with any `#MODBUS_*` directive, the adapter starts with an **empty mapping** (a warning is logged).

Inspect the resulting map at runtime:

```bash
curl http://gateway:8000/registry/modbus | jq
```

### Supported Function Codes

| FC | Name | Description |
|----|------|-------------|
| 01 | Read Coils | Read discrete output coils |
| 02 | Read Discrete Inputs | Read discrete input contacts |
| 03 | Read Holding Registers | Read output registers (16-bit) |
| 04 | Read Input Registers | Read input registers (16-bit) |
| 05 | Write Single Coil | Write a single coil |
| 06 | Write Single Register | Write a single holding register |
| 15 | Write Multiple Coils | Write multiple coils |
| 16 | Write Multiple Registers | Write multiple holding registers |

### Security

Modbus clients are validated by source IP against the `PROTOCOL_MODBUS` rules of the policy engine. See [security.md](security.md) for the full policy reference.
