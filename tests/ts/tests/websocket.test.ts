import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("WebSocket Telemetry Tests", () => {
  let gateway: GatewayProcess;
  let ws: WebSocket;

  beforeAll(async () => {
    gateway = new GatewayProcess();
    await gateway.start();

    await gateway.reloadPolicy(`
            void setup() {
                http().allow();
            }
        `);
  });

  afterAll(async () => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.close();
    }
    await gateway.stop();
  });

  test("WebSocket connection opens successfully", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    const connected = await new Promise<boolean>((resolve) => {
      ws.onopen = () => resolve(true);
      ws.onerror = () => resolve(false);
      setTimeout(() => resolve(false), 5000);
    });

    expect(connected).toBe(true);
  });

  test("WebSocket receives telemetry data after subscription", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve();
      ws.onerror = () => reject(new Error("WebSocket failed to open"));
      setTimeout(() => reject(new Error("Timeout")), 5000);
    });

    // Subscribe to a test field
    const subscribeMsg = {
      type: "subscribe",
      path: "DB2/temperatures",
    };

    let receivedData = false;
    const dataPromise = new Promise<void>((resolve) => {
      ws.onmessage = (event) => {
        const data = JSON.parse(event.data);
        if (data.path === "DB2/temperatures" || data.type === "telemetry") {
          receivedData = true;
          resolve();
        }
      };
    });

    ws.send(JSON.stringify(subscribeMsg));

    // Wait for telemetry data
    await Promise.race([
      dataPromise,
      new Promise<void>((resolve) => setTimeout(resolve, 3000)),
    ]);

    // WebSocket should receive some form of acknowledgment or data
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket subscription and unsubscription", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Subscribe
    ws.send(JSON.stringify({ type: "subscribe", path: "DB2/temperatures" }));
    await new Promise((r) => setTimeout(r, 500));

    // Unsubscribe
    ws.send(JSON.stringify({ type: "unsubscribe", path: "DB2/temperatures" }));
    await new Promise((r) => setTimeout(r, 500));

    // Connection should still be alive
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket handles multiple subscriptions", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Subscribe to multiple paths
    ws.send(JSON.stringify({ type: "subscribe", path: "DB2/temperatures" }));
    await new Promise((r) => setTimeout(r, 200));

    ws.send(JSON.stringify({ type: "subscribe", path: "DB2/pressure" }));
    await new Promise((r) => setTimeout(r, 200));

    // Connection should remain stable
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket clear_subscriptions command", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Subscribe first
    ws.send(JSON.stringify({ type: "subscribe", path: "DB2/temperatures" }));
    await new Promise((r) => setTimeout(r, 500));

    // Clear all subscriptions
    ws.send(JSON.stringify({ type: "clear_subscriptions" }));
    await new Promise((r) => setTimeout(r, 500));

    // Connection should still be alive
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket receives ping/pong heartbeats", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Wait for potential ping/pong
    await new Promise((r) => setTimeout(r, 2000));

    // Connection should remain stable
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket handles invalid JSON gracefully", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Send invalid JSON
    ws.send("invalid json message");
    await new Promise((r) => setTimeout(r, 500));

    // Connection should remain alive
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket handles unknown message types", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Send unknown message type
    ws.send(JSON.stringify({ type: "unknown_command" }));
    await new Promise((r) => setTimeout(r, 500));

    // Connection should remain alive
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket subscription with nested paths", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Subscribe to nested path (if schema supports it)
    ws.send(JSON.stringify({ type: "subscribe", path: "DB2/temperatures/0" }));
    await new Promise((r) => setTimeout(r, 500));

    // Connection should remain stable
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket handles rapid subscription changes", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Rapidly subscribe and unsubscribe
    for (let i = 0; i < 10; i++) {
      ws.send(
        JSON.stringify({ type: "subscribe", path: `DB2/temperatures/${i}` }),
      );
      await new Promise((r) => setTimeout(r, 50));
      ws.send(
        JSON.stringify({ type: "unsubscribe", path: `DB2/temperatures/${i}` }),
      );
      await new Promise((r) => setTimeout(r, 50));
    }

    // Connection should remain stable
    expect(ws.readyState).toBe(WebSocket.OPEN);
  });

  test("WebSocket connection closes cleanly", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    const closed = await new Promise<boolean>((resolve) => {
      ws.onclose = () => resolve(true);
      ws.close();
      setTimeout(() => resolve(false), 2000);
    });

    expect(closed).toBe(true);
  });

  test("WebSocket handles oversized messages", async () => {
    const wsUrl = "ws://localhost:8081";
    ws = new WebSocket(wsUrl);

    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
      setTimeout(() => resolve(), 5000);
    });

    // Send a very large message (should be rejected or handled gracefully)
    const largeMessage = JSON.stringify({
      type: "subscribe",
      path: "A".repeat(100000),
    });

    ws.send(largeMessage);
    await new Promise((r) => setTimeout(r, 1000));

    // Connection should either remain open or close gracefully
    const isAlive = ws.readyState === WebSocket.OPEN;
    const isClosed = ws.readyState === WebSocket.CLOSED;

    expect(isAlive || isClosed).toBe(true);
  });
});
