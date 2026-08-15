## Security Policy

The gateway ships a two-mode security model controlled by `security_policy` in `gateway.json`.

### Policy Modes

| Mode | Behaviour |
|------|-----------|
| `Relaxed` | No access control. All clients accepted. Suitable for development. |
| `Strict` | AngelScript rule engine loaded from `security_script`. All requests evaluated against active rules. |

### AngelScript Policy Engine

The security policy is defined in an AngelScript (`.as`) file. The gateway loads and compiles it at startup. The script calls a fluent builder API to define rules.

**Example (`security.as`):**

```angelscript
void definePolicy(PolicyBuilder@ policy) {
    // Allow S7 from the plant LAN only
    policy
        .forProtocol(PROTOCOL_S7)
        .fromCidr("10.0.0.0/8")
        .allow();

    // Allow HTTP from SCADA server
    policy
        .forProtocol(PROTOCOL_HTTP)
        .fromCidr("10.0.1.50")
        .requireHeader("X-SGRN-Key")
        .allow();

    // Allow WebSocket from allowed origins
    policy
        .forProtocol(PROTOCOL_WEBSOCKET)
        .allowOrigins({"http://hmi.plant.local", "http://10.0.1.100"})
        .allow();

    // Allow OPC-UA from historian only
    policy
        .forProtocol(PROTOCOL_OPCUA)
        .fromCidr("10.0.2.10")
        .allowSessionNames({"HISTORIAN_MAIN"})
        .allow();
}
```

### Rule Predicates

| Builder Method | Description |
|----------------|-------------|
| `.forProtocol(P)` | Match only the specified protocol |
| `.fromCidr("x.x.x.x/n")` | Match source IP against CIDR range |
| `.fromIp("x.x.x.x")` | Match exact source IP |
| `.allowDbNumbers({N, M})` | Restrict to specific DB numbers |
| `.requireHeader("H")` | Require an HTTP header to be present |
| `.allowOrigins({"url"})` | Match HTTP/WS `Origin` header |
| `.allowSessionNames({"name"})` | Match OPC-UA session name |
| `.allow()` | Emit an ALLOW rule (default deny-by-default) |
| `.deny()` | Emit an explicit DENY rule |

### Protocol Constants

| Constant | Protocol |
|----------|----------|
| `PROTOCOL_S7` | S7 ISO-on-TCP (port 102) |
| `PROTOCOL_HTTP` | REST / HTTP (northbound) |
| `PROTOCOL_WEBSOCKET` | WebSocket telemetry |
| `PROTOCOL_MODBUS` | Modbus TCP |
| `PROTOCOL_OPCUA` | OPC Unified Architecture |
| `PROTOCOL_ETHERNETIP` | EtherNet/IP (CIP) |

### Rule Evaluation

Rules are evaluated in order of **specificity** (most specific first). A rule matching a client IP/protocol terminates the chain. If no rule matches, the default decision is **DENY**.

Inspect the active policy at any time from the web UI ([Policy tab](#/policy)) or via the API:

```bash
curl http://gateway:8000/api/policy | jq
```
