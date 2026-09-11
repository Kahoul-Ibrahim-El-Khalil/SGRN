# gateway

Main SGRN gateway executable: the multi-protocol telemetry hub that shadows a
Siemens S7 PLC into an in-memory digital twin and projects it northbound.

## Role

Runs the full `GatewayApplication` lifecycle: load config → load schema →
init security → init twin → init threading → wire telemetry → init
infrastructure → start adapters → feed initial anchor → run. Southbound it
speaks S7 (Snap7), Modbus, EtherNet/IP and OPC UA; northbound it serves HTTP,
WebSocket, OPC UA and the persistence/datastore bridge. See
`GatewayApplication` in `sgrn/lib/gateway/include/sgrn/gateway/gateway.hpp`
for the stage breakdown.

## Usage

```text
gateway <config.json>
gateway --generate-config -o <config.json>
```

```bash
gateway sgrn/lib/gateway/simulations/nuclear/gateway.json
```

The process runs until SIGINT/SIGTERM (`sgrn::utils::app::runMain` handles
signals and exit codes). A `--help`-style usage string is printed when the
config step fails.

## Configuration

Everything comes from the JSON config file (adapters, ports, schemas,
security policy script, persistence). Schema example:
`sgrn/lib/gateway/simulations/nuclear/`.

## Dependencies

Links `sgrn_gateway` (orchestration + HTTP/WebSocket), `sgrn_gateway_modbus`
and `sgrn_gateway_ethernetip` (linked explicitly so their static
registrations survive `--as-needed`), plus Threads, zstd and OpenSSL.
Builds with the `sgrn_pch_s7` precompiled header.
