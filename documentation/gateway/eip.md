## EtherNet/IP

The gateway acts as an EtherNet/IP adapter (CIP server), exposing PLC Data Blocks as CIP tags accessible to Allen-Bradley PLCs and other EtherNet/IP originators.

### Configuration

The `listen.ethernetip` block selects the CIP server bind address and port:

```json
{
  "listen": {
    "ethernetip": {
      "ip": "0.0.0.0",
      "port": 44818
    }
  }
}
```

### CIP Tag Mapping

Each DB field maps to a CIP tag using the naming convention `<DBName>.<FieldName>`:

| CIP Tag | DB | Field | Type |
|---------|----|-------|------|
| `ReactorCore.temperature` | DB1 | temperature | REAL |
| `ReactorCore.status_word` | DB1 | status_word | INT |
| `PumpStation.flow_rate` | DB2 | flow_rate | REAL |

### Supported CIP Services

| Service | Code | Description |
|---------|------|-------------|
| Get_Attribute_Single | 0x0E | Read a single tag |
| Set_Attribute_Single | 0x10 | Write a single tag |
| Read_Tag | 0x4C | Read tag value |
| Write_Tag | 0x4D | Write tag value |

### Security

EtherNet/IP clients are validated by source IP against the `PROTOCOL_ETHERNETIP` rules of the policy engine. See [security.md](security.md) for the full policy reference.
