## OPC-UA

The gateway exposes all PLC Data Blocks as OPC-UA nodes, enabling integration with SCADA systems, historians, and OPC-UA clients.

### Configuration

The `listen.opcua` block selects the server bind address and port:

```json
{
  "listen": {
    "opcua": {
      "ip": "0.0.0.0",
      "port": 4840
    }
  }
}
```

### Node Structure

Each DB is mapped to an OPC-UA object node, and each field becomes a variable node:

```
Objects/
  └── SGRN/
        ├── ReactorCore/
        │     ├── temperature  (Double)
        │     ├── pressure     (Double)
        │     └── status_word  (UInt16)
        └── PumpStation/
              └── flow_rate    (Double)
```

### Subscriptions

OPC-UA clients can subscribe to any variable node. The gateway translates WebSocket delta updates into OPC-UA `DataChange` notifications, forwarding them to all active OPC-UA subscriptions at each update cycle. Aggregate `.Value` variables (whole-DB or struct snapshot) refresh whenever a child changes.

### Security Policy Integration

When the security policy is `strict`, OPC-UA sessions must match the `allow_session_names` predicate of an active rule; anonymous sessions are rejected unless an `ALLOW` rule without a session predicate exists. See [security.md](security.md) for the builder API and a full example.
