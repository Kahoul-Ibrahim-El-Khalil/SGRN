# mbproxy

Modbus proxy: polls source Modbus devices and pushes changed registers/coils
into a central Modbus hub. Lets multiple PLCs/masters share one hub map
without touching the SGRN twin — this tool talks raw Modbus only.

## Role

For each mapping in the config, one `MbProxySession` runs an asio timer
loop: read source registers/coils → compare against last state → on change,
write to the hub address. Unchanged values are suppressed (counted and
periodically reported in verbose mode); failed reads/writes disconnect so
the next tick reconnects. Runs until SIGINT/SIGTERM.

## Usage

```text
Usage: mbproxy <config.json>
```

```json
{
    "hub_ip": "127.0.0.1",
    "hub_port": 502,
    "verbose": false,
    "devices": [
        {
            "name": "plc1",
            "ip": "192.168.0.10",
            "port": 502,
            "mappings": [
                { "type": "holding", "src_address": 0, "dst_address": 100, "count": 10, "interval_ms": 100 },
                { "type": "coil", "src_address": 0, "dst_address": 0, "count": 8, "interval_ms": 250 }
            ]
        }
    ]
}
```

Field reference (`sgrn::gateway::config::MbProxyConfig`): mapping `type` is
`"holding"` or `"coil"`; `count` is registers (2 bytes each) or coils;
`interval_ms` is the per-mapping poll period. A 4-thread asio pool serves
all sessions; a missing client or empty session list exits non-zero.

## Dependencies

Links `sgrn_gateway` (config parsing) and `sgrn::wrappers::modbus`
(Modbus TCP client), plus Threads. Builds with the `sgrn_pch_s7`
precompiled header.
