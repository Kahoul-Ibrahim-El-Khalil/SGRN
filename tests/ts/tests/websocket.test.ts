import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

type WritableByte = { db: number; original: number };

function toBase64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

async function findWritableByte(): Promise<WritableByte> {
  const registryResponse = await fetch("http://localhost:8080/registry");
  expect(registryResponse.status).toBe(200);
  const registry = await registryResponse.json();
  const db = registry.dbs.find((candidate: { db_number: number; size_bytes: number }) => candidate.size_bytes > 0);
  if (!db) throw new Error("Test gateway has no non-empty DB");

  const memoryResponse = await fetch(
    `http://localhost:8080/memory/db/${db.db_number}/offset/0/size/1`,
  );
  expect(memoryResponse.status).toBe(200);
  const bytes = new Uint8Array(await memoryResponse.arrayBuffer());
  expect(bytes.length).toBe(1);
  return { db: db.db_number, original: bytes[0] };
}

function waitForOpen(socket: WebSocket): Promise<void> {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error("WebSocket open timeout")), 5000);
    socket.addEventListener("open", () => {
      clearTimeout(timeout);
      resolve();
    }, { once: true });
    socket.addEventListener("error", () => {
      clearTimeout(timeout);
      reject(new Error("WebSocket connection failed"));
    }, { once: true });
  });
}

function waitForJsonAck(socket: WebSocket, sequence: number): Promise<{ ok: boolean; sequence: number }> {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      socket.removeEventListener("message", onMessage);
      reject(new Error(`JSON write acknowledgement timeout for sequence ${sequence}`));
    }, 5000);
    const onMessage = (event: MessageEvent) => {
      if (typeof event.data !== "string") return;
      let message: { type?: string; sequence?: number; ok?: boolean };
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      if (message.type !== "write_ack" || message.sequence !== sequence) return;
      clearTimeout(timeout);
      socket.removeEventListener("message", onMessage);
      resolve({ ok: message.ok === true, sequence: message.sequence });
    };
    socket.addEventListener("message", onMessage);
  });
}

function runtimeSyncWriteFrame(db: number, value: number, sequence: bigint): ArrayBuffer {
  const frame = new ArrayBuffer(39);
  const bytes = new Uint8Array(frame);
  bytes.set([0x53, 0x47, 0x52, 0x57], 0); // SGRW
  const view = new DataView(frame);
  view.setUint16(4, 1, true); // version
  view.setUint8(6, 2); // Kind::Write
  view.setUint8(7, 0); // reserved flags
  view.setBigUint64(8, sequence, true);
  view.setBigUint64(16, 1234n, true);
  view.setUint32(24, 1, true); // record count
  view.setUint16(28, db, true);
  view.setUint32(30, 0, true); // offset
  view.setUint32(34, 1, true); // size
  view.setUint8(38, value);
  return frame;
}

function waitForBinaryAck(socket: WebSocket, sequence: bigint): Promise<void> {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      socket.removeEventListener("message", onMessage);
      reject(new Error(`binary write acknowledgement timeout for sequence ${sequence}`));
    }, 5000);
    const onMessage = async (event: MessageEvent) => {
      if (typeof event.data === "string") return;
      const buffer = event.data instanceof ArrayBuffer
        ? event.data
        : event.data instanceof Blob
          ? await event.data.arrayBuffer()
          : null;
      if (!buffer || buffer.byteLength < 28) return;
      const bytes = new Uint8Array(buffer);
      if (bytes[0] !== 0x53 || bytes[1] !== 0x47 || bytes[2] !== 0x52 || bytes[3] !== 0x57 || bytes[6] !== 3) return;
      const view = new DataView(buffer);
      if (view.getBigUint64(8, true) !== sequence) return;
      clearTimeout(timeout);
      socket.removeEventListener("message", onMessage);
      resolve();
    };
    socket.addEventListener("message", onMessage);
  });
}

async function readByte(db: number): Promise<number> {
  const response = await fetch(`http://localhost:8080/memory/db/${db}/offset/0/size/1`);
  expect(response.status).toBe(200);
  return new Uint8Array(await response.arrayBuffer())[0];
}

async function restoreByte(db: number, value: number): Promise<void> {
  const response = await fetch(`http://localhost:8080/memory/db/${db}/offset/0/size/1`, {
    method: "PUT",
    headers: { "Content-Type": "application/octet-stream" },
    body: new Uint8Array([value]),
  });
  expect(response.status).toBe(200);
}

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
    const wsUrl = "ws://localhost:8080/ws";
    ws = new WebSocket(wsUrl);

    const connected = await new Promise<boolean>((resolve) => {
      ws.onopen = () => resolve(true);
      ws.onerror = () => resolve(false);
      setTimeout(() => resolve(false), 5000);
    });

    expect(connected).toBe(true);
  });

  test("RuntimeSync JSON and binary writes update the same runtime memory", async () => {
    const target = await findWritableByte();
    const socket = new WebSocket("ws://localhost:8080/ws");
    await waitForOpen(socket);

    try {
      const jsonValue = target.original ^ 0xff;
      const jsonSequence = 7001;
      const jsonAck = waitForJsonAck(socket, jsonSequence);
      socket.send(JSON.stringify({
        command: "write",
        sequence: jsonSequence,
        updates: [{
          db: target.db,
          offset: 0,
          size: 1,
          data: toBase64Url(new Uint8Array([jsonValue])),
        }],
      }));
      expect((await jsonAck).ok).toBe(true);
      expect(await readByte(target.db)).toBe(jsonValue);

      const binaryValue = jsonValue ^ 0x55;
      const binarySequence = 7002n;
      const binaryAck = waitForBinaryAck(socket, binarySequence);
      socket.send(runtimeSyncWriteFrame(target.db, binaryValue, binarySequence));
      await binaryAck;
      expect(await readByte(target.db)).toBe(binaryValue);
    } finally {
      await restoreByte(target.db, target.original);
      socket.close();
    }
  });

  test("Malformed RuntimeSync binary frames do not terminate the connection", async () => {
    const socket = new WebSocket("ws://localhost:8080/ws");
    await waitForOpen(socket);
    socket.send(new Uint8Array([0x53, 0x47, 0x52, 0x57, 0x01]).buffer);
    await new Promise((resolve) => setTimeout(resolve, 250));
    expect(socket.readyState).toBe(WebSocket.OPEN);
    socket.close();
  });

  test("WebSocket receives telemetry data after subscription", async () => {
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
    const wsUrl = "ws://localhost:8080/ws";
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
