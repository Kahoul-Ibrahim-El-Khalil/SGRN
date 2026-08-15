# SGRN Gateway TypeScript Tests

Comprehensive test suite for the SGRN Gateway covering HTTP API, WebSocket telemetry, Dashboard functionality, and southbound protocol adapters (S7, Modbus, OPC-UA).

## Prerequisites

- **Bun Runtime** (v1.0+): Tests use `bun:test` framework
  ```bash
  # Install Bun
  curl -fsSL https://bun.sh/install | bash
  ```

- **Built Gateway Binary**: The gateway must be built before running tests
  ```bash
  cd /path/to/PFE
  cmake --build .build/linux-static-release --target gateway
  ```

- **Test Configuration**: Tests automatically configure the gateway with:
  - HTTP port: 8080
  - WebSocket port: 8081
  - S7 port: 8102
  - OPC-UA port: 8480

## Test Structure

```
tests/ts/
├── src/
│   └── GatewayProcess.ts      # Test harness for managing gateway lifecycle
├── tests/
│   ├── registry.test.ts       # Registry endpoint tests (existing)
│   ├── data.test.ts          # Data endpoint tests (existing)
│   ├── policy.test.ts        # Security policy tests (existing)
│   ├── gateway-api.test.ts   # Comprehensive HTTP API tests (NEW)
│   ├── websocket.test.ts     # WebSocket telemetry tests (NEW)
│   ├── dashboard.test.ts     # Dashboard serving tests (NEW)
│   └── southbound-protocols.test.ts  # S7/Modbus/OPC-UA tests (NEW)
└── package.json
```

## Running Tests

### Run All Tests
```bash
cd sgrn/gateway/tests/ts
bun test
```

### Run Specific Test File
```bash
bun test tests/gateway-api.test.ts
bun test tests/websocket.test.ts
bun test tests/southbound-protocols.test.ts
```

### Run with Verbose Output
```bash
bun test --verbose
```

### Run Specific Test Case
```bash
bun test --test-name="GET /registry returns valid schema"
```

## Test Categories

### 1. Gateway API Tests (`gateway-api.test.ts`)
Comprehensive HTTP endpoint testing:
- **Registry Endpoints**: `/registry`, `/registry/types`, `/registry/modbus`
- **Data Endpoints**: CRUD operations on `/data/{DbName}/{Path}`
- **Memory Endpoints**: Binary read/write via `/memory/*`
- **Diagnostic Endpoints**: `/endpoints`, `/connections`, `/db/*`
- **Security Policy**: ACL enforcement, IP allowlists
- **Error Handling**: 404s, 400s, invalid inputs
- **CORS**: Cross-origin headers validation
- **Concurrent Requests**: Multi-threaded access patterns
- **Data Integrity**: Array operations, nested structures
- **Performance**: Load testing, large payloads

### 2. WebSocket Tests (`websocket.test.ts`)
Real-time telemetry streaming:
- Connection lifecycle (open/close)
- Subscription management (subscribe/unsubscribe)
- Multiple concurrent subscriptions
- `clear_subscriptions` command
- Invalid message handling
- Oversized message protection
- Rapid subscription changes
- Heartbeat/ping-pong (implicit)

### 3. Dashboard Tests (`dashboard.test.ts`)
Web UI serving:
- HTML serving at `/`
- Asset accessibility
- CORS headers for development
- JavaScript/CSS loading
- Meta tags and HTML structure
- HTTP method support (HEAD, OPTIONS)

### 4. Southbound Protocol Tests (`southbound-protocols.test.ts`)
PLC protocol adapter testing:
- **S7**: Connection tracking, data access, DB structure
- **Modbus**: Virtual mapping, registry integration
- **OPC-UA**: Connection tracking, configuration validation
- **Multi-Protocol**: Unified API access, telemetry streaming
- **Error Handling**: Invalid DBs, malformed requests
- **Configuration**: Config file validation, schema loading
- **Performance**: Rapid requests, concurrent access

## Test Harness (`GatewayProcess.ts`)

The `GatewayProcess` class manages the gateway lifecycle for tests:

```typescript
const gateway = new GatewayProcess();
await gateway.start();                    // Starts gateway on test ports
await gateway.reloadPolicy(policyStr);    // Hot-reloads security policy
await gateway.stop();                      // Stops gateway and cleans up
```

### Features:
- Automatic port configuration (avoids conflicts)
- Policy hot-reload via SIGHUP
- Backup/restore of security policy
- Configurable test configuration
- Ready-wait with timeout

## Writing New Tests

### Example: Testing a New Endpoint
```typescript
test("GET /api/new-endpoint returns valid data", async () => {
  const res = await fetch("http://localhost:8080/api/new-endpoint");
  expect(res.status).toBe(200);
  
  const data = await res.json();
  expect(data).toHaveProperty("expected_field");
});
```

### Example: Testing WebSocket Messages
```typescript
test("Custom WebSocket command", async () => {
  const ws = new WebSocket("ws://localhost:8081");
  await new Promise(r => ws.onopen = r);
  
  ws.send(JSON.stringify({ type: "custom_command", params: {} }));
  
  const response = await new Promise(resolve => {
    ws.onmessage = (e) => resolve(JSON.parse(e.data));
  });
  
  expect(response.status).toBe("ok");
  ws.close();
});
```

## Troubleshooting

### Gateway Fails to Start
- Check if ports 8080, 8081, 8102, 8480 are available
- Verify gateway binary exists at `.build/linux-static-release/sgrn/gateway/gateway`
- Check gateway logs in test output

### Tests Timeout
- Increase timeout in test: `test("name", async () => { ... }, 10000)` (10s)
- Check if gateway is responding: `curl http://localhost:8080/endpoints`

### Permission Denied
- Ensure gateway binary is executable: `chmod +x .build/linux-static-release/sgrn/gateway/gateway`

## CI/CD Integration

Tests are designed to run in CI environments:

```yaml
# Example GitHub Actions step
- name: Build Gateway
  run: cmake --build .build/linux-static-release --target gateway

- name: Run Tests
  run: |
    cd sgrn/gateway/tests/ts
    bun test
```

## Notes

- Tests assume **no actual PLC connection** (S7/Modbus/OPC-UA servers may not be present)
- Tests validate gateway behavior with and without protocol connections
- Some tests are informational (logging data presence) rather than strict assertions
- Policy tests hot-reload the security script; failures restore the original policy

## Future Enhancements

- [ ] Add mock PLC simulators for offline testing
- [ ] Add WebSocket telemetry data validation
- [ ] Add stress/load testing suite
- [ ] Add protocol-specific integration tests with simulators
- [ ] Add code coverage reporting
- [ ] Add test fixtures for common scenarios