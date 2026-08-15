# Gateway Binding HTTP Payload

`GatewaySync`/`GatewayBinding` uses WebSocket only for inbound Gateway
`DeltaSnapshot` messages and subscription commands. Gateway-side
`WebSocketFacade::handleClientMessage()` accepts only:

- `{"command":"subscribe","path":"..."}`
- `{"command":"unsubscribe","path":"..."}`

Shell-to-Gateway writes therefore use `PUT /memory/batch`. The Gateway handler
expects a JSON array where each item is:

```json
{"db":1,"offset":0,"size":4,"data":"base64url-bytes"}
```

This endpoint was chosen over `/data/<path>` because `PlcRuntime` already
tracks dirty byte regions by DB, offset and length. A single `/memory/batch`
request can publish multiple dirty regions, including regions from different
DBs, without inventing a second payload shape or dirty ledger.
