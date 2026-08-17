# @sgrn/gateway

TypeScript bindings for the SGRN Gateway.

## Import

```ts
import {
  fetchRegistryHeaders,
  fetchFullRegistry,
  fetchSecurityPolicy,
  buildRegistryTree,
  buildEipProjection,
  buildOpcuaProjection,
  matchSecurityRule,
  GatewayClient,
  ws,
} from "@sgrn/gateway";
```

## What it exposes

- REST helpers for registry, policy, and Modbus data
- WebSocket telemetry client
- Registry tree and projection helpers
- Security rule matching helpers
- Shared gateway types and DTOs

## Notes

- The package is source-first and intended to be consumed directly from Bun or frontend apps.
- The app packages in this repo depend on it through local `file:` references.
