# Gateway Binding RuntimeSync Payload

`GatewaySync` uses one WebSocket session for inbound Gateway `DeltaSnapshot`
messages, subscriptions, and outbound dirty-region writes. The inspectable
JSON command is:

```json
{
  "command": "write",
  "sequence": 7,
  "updates": [
    {"db":1,"offset":0,"size":4,"data":"base64url-bytes"}
  ]
}
```

The gateway replies with `{"type":"write_ack","sequence":7,"ok":true}`.
`GatewaySync.useBinary(true)` sends the same updates as a versioned `SGRW`
binary frame and waits for its binary acknowledgement.

The payload remains raw DB bytes because `PlcRuntime` already tracks dirty
regions by DB, offset, and length; typed clients use the negotiated SCL schema
to decode those bytes without losing PLC endianness or padding.

## Discrete control writes (`write_area`)

Discrete tags (`%I/%Q/%M` arenas) publish through a parallel JSON command —
always JSON, even in binary mode, since control traffic is low-rate and stays
inspectable:

```json
{
  "command": "write_area",
  "sequence": 8,
  "updates": [
    {"area":130,"offset":0,"size":1,"data":"AQ"}
  ]
}
```

`area` is the S7 area code (129 = PE, 130 = PA, 131 = MK). The gateway
applies the bytes to its runtime arenas (overlapping tags go dirty and
re-broadcast) and replies with the same `write_ack` shape. Servers without
area support (full gateway: twin is DB-only) NACK with a clear error; on ack
timeout `GatewaySync` falls back to `POST /tags/<name>` per tag.

Delivery model: tag writes record into two ledgers — `takeDirtyTags()` for
gateway broadcasts, `takePublishTags()` (+ restore on NACK/timeout) for the
reliable uplink — so a shell serving dashboards and uplinking never starves
either consumer. Inbound (gateway-originated) writes are suppression-flagged
like DB deltas.

Loop safety: uplink applies on the gateway side are *silent*
(`writeAreaMemory(..., mark=false)`), exactly like twin DB writes, which
never touch the runtime dirty ledger. A marking apply would broadcast every
uplinked write back to its sender, refilling the sync's publish ledger from
its own echo — a self-sustaining republish loop. Local viewers converge on
the next change, same as DBs today.
