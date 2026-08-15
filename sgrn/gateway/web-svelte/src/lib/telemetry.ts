// ─────────────────────────────────────────────────────────────────────────────
// telemetry.ts — GatewayClient WebSocket wrapper (ES module)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * NOTE: This enum must be kept in manually sync with @sgrn/types ErrorScope 
 * as this project doesn't have a shared dependency on it.
 */
enum ErrorScope {
  Network = "Network",
  Runtime = "Runtime",
}

interface SgrnResult<T> {
  data?: T;
  error?: string;
  scope?: ErrorScope;
}

type RawMessageCallback = (data: unknown) => void;
type UpdateCallback = (data: unknown) => void;
type StatusCallback = (status: string) => void;

export class GatewayClient {
  private socket: WebSocket | null = null;
  private connected: boolean = false;
  private subscriptions: Set<string> = new Set();
  private listeners: Set<UpdateCallback> = new Set();
  private rawListeners: Set<RawMessageCallback> = new Set();
  private statusListeners: Set<StatusCallback> = new Set();
  private reconnectInterval: number = 5000;
  private url: string;

  constructor(url: string) {
    this.url = url;
  }

  connect(): SgrnResult<void> {
    try {
      this.socket = new WebSocket(this.url);

      this.socket.onopen = () => {
        this.connected = true;
        this.notifyStatus("CONNECTED");
        this.subscriptions.forEach((path) => this.subscribe(path, true));
      };

      this.socket.onclose = () => {
        this.connected = false;
        this.notifyStatus("DISCONNECTED");
        setTimeout(() => this.connect(), this.reconnectInterval);
      };

      this.socket.onerror = (err: Event) => {
        this.notifyStatus("ERROR: Connection Refused or SSL Blocked");
        console.error("S7Gateway WebSocket Error:", err);
      };

      this.socket.onmessage = (event: MessageEvent<string>) => {
        try {
          const data: unknown = JSON.parse(event.data);
          this.rawListeners.forEach((cb) => cb(data));
          // Legacy envelope support
          if (data && typeof data === "object") {
            const d = data as Record<string, unknown>;
            if (d["updates"] && Array.isArray(d["updates"])) {
              (d["updates"] as unknown[]).forEach((u) => this.notifyUpdate(u));
              return;
            }
            if (
              d["event"] === "update" ||
              (d["db"] !== undefined && d["value"] !== undefined)
            ) {
              this.notifyUpdate(data);
            }
          }
        } catch (e) {
          console.warn("Non-JSON message received:", event.data);
        }
      };

      return { data: undefined };
    } catch (e) {
      return {
        error: `Failed to initialize WebSocket: ${e instanceof Error ? e.message : String(e)}`,
        scope: ErrorScope.Network,
      };
    }
  }

  subscribe(path: string, force: boolean = false): SgrnResult<void> {
    if (!force && this.subscriptions.has(path)) return { data: undefined };
    this.subscriptions.add(path);
    if (!this.socket || !this.connected) {
      return { data: undefined }; // Will be sent automatically on onopen
    }
    try {
      this.socket.send(JSON.stringify({ command: "subscribe", path }));
      return { data: undefined };
    } catch (e) {
      return {
        error: `Failed to send subscribe: ${e instanceof Error ? e.message : String(e)}`,
        scope: ErrorScope.Runtime,
      };
    }
  }

  unsubscribe(path: string): SgrnResult<void> {
    this.subscriptions.delete(path);
    if (!this.socket || !this.connected) {
      return { error: "WebSocket is not connected", scope: ErrorScope.Network };
    }
    try {
      this.socket.send(JSON.stringify({ command: "unsubscribe", path }));
      return { data: undefined };
    } catch (e) {
      return {
        error: `Failed to send unsubscribe: ${e instanceof Error ? e.message : String(e)}`,
        scope: ErrorScope.Runtime,
      };
    }
  }

  clearSubscriptions(): SgrnResult<void> {
    this.subscriptions.clear();
    if (!this.socket || !this.connected) {
      return { error: "WebSocket is not connected", scope: ErrorScope.Network };
    }
    try {
      this.socket.send(JSON.stringify({ command: "clear_subscriptions" }));
      return { data: undefined };
    } catch (e) {
      return {
        error: `Failed to clear subscriptions: ${e instanceof Error ? e.message : String(e)}`,
        scope: ErrorScope.Runtime,
      };
    }
  }

  onRawMessage(callback: RawMessageCallback): () => boolean {
    this.rawListeners.add(callback);
    return () => this.rawListeners.delete(callback);
  }

  onUpdate(callback: UpdateCallback): () => boolean {
    this.listeners.add(callback);
    return () => this.listeners.delete(callback);
  }

  onStatusChange(callback: StatusCallback): () => boolean {
    this.statusListeners.add(callback);
    callback(this.connected ? "CONNECTED" : "DISCONNECTED");
    return () => this.statusListeners.delete(callback);
  }

  private notifyUpdate(data: unknown): void {
    this.listeners.forEach((cb) => cb(data));
  }

  private notifyStatus(status: string): void {
    this.statusListeners.forEach((cb) => cb(status));
  }
}
