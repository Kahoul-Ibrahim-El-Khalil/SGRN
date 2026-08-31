// ─────────────────────────────────────────────────────────────────────────────
// worker.ts — SGRN Industrial Telemetry Worker
// Handles high-frequency WebSocket data in a background thread.
// ─────────────────────────────────────────────────────────────────────────────
import { GatewayClient, type WorkerCommand, type WorkerMessage } from "@sgrn/gateway";

let validKeys: Set<string> | null = null;
let client: GatewayClient | null = null;
const updateBuffer: Map<string, { value: unknown; ts: number }> = new Map();
let FLUSH_INTERVAL = 32; // ~30 Hz flush rate for UI smoothness
let flushTimer: ReturnType<typeof setTimeout> | null = null;
const activeSubscriptions = new Set<string>();

/** Checks if a given db or db/path is covered by any active subscription. */
function isSubscribed(db: string, path: string): boolean {
  if (activeSubscriptions.size === 0) return false;

  const fullPath = path ? `${db}/${path}` : db;
  if (activeSubscriptions.has(fullPath)) return true;

  for (const sub of activeSubscriptions) {
    if (fullPath.startsWith(sub + "/") || sub === db) {
      return true;
    }
  }
  return false;
}

/** Recursively flattens nested telemetry objects into a flat key-value map. */
function flattenTelemetry(
  db: string,
  path: string,
  value: unknown,
  ts: number,
): void {
  const fullKey = `${db}-${path}`;
  if (value === null || value === undefined) {
    if (!isSubscribed(db, path)) return;
    if (path !== "" && validKeys && !validKeys.has(fullKey)) return;
    updateBuffer.set(fullKey, { value: null, ts });
    return;
  }
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i++) {
      const subPath = path ? `${path}/[${i}]` : `[${i}]`;
      flattenTelemetry(db, subPath, value[i], ts);
    }
  } else if (typeof value === "object") {
    for (const k in value as Record<string, unknown>) {
      const subPath = path ? `${path}/${k}` : k;
      flattenTelemetry(db, subPath, (value as Record<string, unknown>)[k], ts);
    }
  } else {
    if (!isSubscribed(db, path)) return;
    if (path !== "" && validKeys && !validKeys.has(fullKey)) return;
    updateBuffer.set(fullKey, { value, ts });
  }
}

function flush(): void {
  if (updateBuffer.size === 0) return;

  const updates: Record<string, { value: unknown; ts: number }> = {};
  updateBuffer.forEach((v, k) => {
    updates[k] = v;
  });

  const msg: WorkerMessage = { type: "batch", updates };
  self.postMessage(msg);
  updateBuffer.clear();
}

(self as unknown as Worker).onmessage = function (
  e: MessageEvent<WorkerCommand>,
): void {
  const { command, args } = e.data;

  if (command === "connect") {
    const { url } = args;
    client = new GatewayClient(url);
    validKeys = null;

    client.onRawMessage((data: unknown) => {
      const debugMsg: WorkerMessage = {
        type: "debug",
        args: {
          msg: `RAW RX: ${JSON.stringify(data).substring(0, 80)}...`,
          color: "#666",
        },
      };
      self.postMessage(debugMsg);

      if (!data || typeof data !== "object") return;
      const ts = Date.now();

      for (const dbName in data as Record<string, unknown>) {
        const dbData = (data as Record<string, unknown>)[dbName];
        if (typeof dbData !== "object" || dbData === null) continue;
        flattenTelemetry(dbName, "", dbData, ts);
      }

      if (flushTimer !== null) clearTimeout(flushTimer);
      flushTimer = setTimeout(flush, FLUSH_INTERVAL);
    });

    client.onStatusChange((status: string) => {
      const msg: WorkerMessage = { type: "status", status };
      self.postMessage(msg);
    });

    client.connect();

    const connectMsg: WorkerMessage = {
      type: "debug",
      args: { msg: `Worker connecting to ${url}`, color: "var(--accent)" },
    };
    self.postMessage(connectMsg);
  } else if (command === "subscribe") {
    const path = String(args.path);
    activeSubscriptions.add(path);
    const debugMsg: WorkerMessage = {
      type: "debug",
      args: { msg: `Subscribing to ${path}`, color: "#fff" },
    };
    self.postMessage(debugMsg);
    client?.subscribe(path);
  } else if (command === "unsubscribe") {
    const path = String(args.path);
    activeSubscriptions.delete(path);
    client?.unsubscribe(path);
  } else if (command === "clear_subscriptions") {
    activeSubscriptions.clear();
    client?.clearSubscriptions();
    updateBuffer.clear();
  } else if (command === "setFlushInterval") {
    const ms = Math.max(8, Math.min(1000, args.ms));
    FLUSH_INTERVAL = ms;
    const msg: WorkerMessage = {
      type: "debug",
      args: {
        msg: `Flush interval → ${ms}ms (~${Math.round(1000 / ms)} Hz)`,
        color: "var(--accent)",
      },
    };
    self.postMessage(msg);
  }
};
