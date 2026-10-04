# TypeScript gateway tests

Run these from the repository root. They use Bun and each test file starts and stops its own gateway fixture.

```sh
python3 tests/run_tests.py --kind ts             # all TypeScript test files
python3 tests/run_tests.py ts-websocket          # one file through the main runner
cd tests/ts && bun test tests/websocket.test.ts  # direct Bun command
```

The main runner reports missing Bun or build prerequisites before attempting to run. The direct Bun command is useful when iterating on a single test, but requires the gateway and s7shell binaries at `.build/linux-static-release/sgrn/apps/gateway/gateway` and `.build/linux-static-release/sgrn/apps/s7shell/s7shell`.

## Test files

- `gateway-api.test.ts`, `data.test.ts`, `registry.test.ts`: HTTP surfaces.
- `policy.test.ts`: policy behavior.
- `websocket.test.ts`: WebSocket telemetry.
- `dashboard.test.ts`: embedded dashboard response.
- `southbound-s7.test.ts`, `southbound-modbus.test.ts`, `southbound-opcua.test.ts`, `southbound-ethernetip.test.ts`: protocol views and adapters.
- `southbound-integration.test.ts`: cross-protocol behavior.
- `s7shell.test.ts`: s7shell command-line behavior.

Run the protocol files directly with `bun test tests/southbound-opcua.test.ts` or `bun test tests/southbound-ethernetip.test.ts`. From `tests/ts/`, the package shortcuts are `bun run test:opcua` and `bun run test:eip`.

The fixture uses HTTP/WebSocket port 8080, S7 port 8102, and OPC-UA port 8480. Those ports must be free. See [`src/GatewayProcess.ts`](src/GatewayProcess.ts) for fixture setup and cleanup.
