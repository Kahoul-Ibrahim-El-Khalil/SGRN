## Introduction

**SGRN Gateway** is an industrial-grade bridge that connects Siemens S7-300/400 PLCs to modern northbound systems via REST, WebSocket, OPC-UA, Modbus TCP, and EtherNet/IP.

The gateway maintains a live **Digital Twin** of the PLC's process image — a semantic, JSON-queryable mirror of every Data Block — and fans it out in real time to connected clients.

### Key Features

- **Protocol-agnostic northbound**: Expose PLC data over REST, WebSocket streaming, OPC-UA, Modbus TCP, EtherNet/IP simultaneously.
- **Digital Twin**: Atomic, versioned, copy-on-write representation of all DB data.
- **Live WebSocket streaming**: Delta snapshots pushed to browsers and industrial clients at configurable rates.
- **Schema Registry**: Type-aware field definitions from SCL files compiled at build time.
- **Security Policy Engine**: AngelScript-driven rule engine with per-protocol, per-CIDR, per-DB access control.
- **Embedded Web UI**: Self-contained dashboard served by the gateway with no external dependencies.

### Deployment

The gateway runs as a single-binary process:

```bash
./gateway gateway.json
```

All configuration — PLC endpoints, northbound ports, security policy, cloud backend — is read from the JSON config file at startup.
