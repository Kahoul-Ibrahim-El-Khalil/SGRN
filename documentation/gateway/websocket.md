## WebSocket

The gateway exposes a live telemetry stream over WebSocket on a dedicated port configured via `listen.websocket.port` (the sample config uses **8001**).

### Connecting

```js
const ws = new WebSocket("ws://gateway:8001");
```

### Subscribe to a DB or Field

After connecting, send a subscription message to receive delta updates for that path:

```json
{ "command": "subscribe", "path": "ReactorCore" }
```

Subscribing to a whole Data Block uses the DB name as the path; subscribing to a field uses the slash-separated dotted path (`ReactorCore/speed`). The server prunes the JSON payload to the requested fields.

To unsubscribe, or to clear all subscriptions:

```json
{ "command": "unsubscribe", "path": "ReactorCore/speed" }
{ "command": "clear_subscriptions" }
```

If no `subscribe` command is sent, the client stays in **firehose mode** and the gateway pushes every dirty snapshot.

### Message Format

The gateway pushes **delta snapshots** containing only changed fields since the last tick, always rooted at the top-level Data Block name:

```json
{
  "ReactorCore": {
    "temperature": 42.75,
    "pressure": 1.013,
    "status_word": 3
  }
}
```

Multiple DBs may appear in a single message if they were dirty on the same tick.

### Worker Integration

The embedded web UI uses a **Web Worker** to handle the WebSocket connection in a background thread. The worker flattens nested delta objects into a flat `"db-field"` map and flushes batched updates to the main thread at a tunable rate (default 32 ms / ~30 Hz).

This prevents telemetry from blocking the UI thread during high-frequency PLC cycles.

### Flush Rate

The flush rate controls how often buffered updates are batched and sent to the UI:

| Interval | Rate | Use Case |
|----------|------|----------|
| 8 ms | ~125 Hz | High-speed monitoring |
| 32 ms | ~30 Hz | Standard UI (default) |
| 100 ms | 10 Hz | Low-power / overview |
| 1000 ms | 1 Hz | Dashboard tiles only |
