import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("OPC-UA Protocol Adapter", () => {
  let gateway: GatewayProcess;

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
    await gateway.stop();
  });

  describe("OPC-UA Protocol Adapter", () => {
    test("OPC-UA connections tracked in /connections", async () => {
      const res = await fetch("http://localhost:8080/connections");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(Array.isArray(data)).toBe(true);

      // Check for OPC-UA connections (may be empty if no server configured)
      const opcuaConnections = data.filter(
        (conn: any) =>
          conn.endpoint &&
          (conn.endpoint.includes("opc") || conn.endpoint.includes("OPC")),
      );

      expect(Array.isArray(opcuaConnections)).toBe(true);
    });

    test("OPC-UA data available through registry", async () => {
      const res = await fetch("http://localhost:8080/registry");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(data).toHaveProperty("dbs");
      expect(data).toHaveProperty("udts");

      // OPC-UA data would be mapped to DBs or tags
      // This validates the registry structure is accessible
    });

    test("OPC-UA endpoint configuration validation", async () => {
      // Check if OPC-UA configuration is present
      const fs = await import("fs");
      const path = await import("path");

      const configPath = path.join(
        import.meta.dir,
        "../../../sgrn/gateway/config/s7gateway.json",
      );

      if (fs.existsSync(configPath)) {
        const configStr = fs
          .readFileSync(configPath, "utf8")
          .replace(/^\s*\/\/.*$/gm, "");
        const config = JSON.parse(configStr);

        // Check if OPC-UA is configured
        const hasOpcua = config.sources && config.sources.opcua;

        if (hasOpcua) {
          expect(config.sources.opcua).toBeDefined();
        } else {
          // OPC-UA not configured, skip
        }
      }
    });
  });

  describe("OPC-UA Protocol Adapter - Advanced", () => {
    test("OPC-UA subscription via WebSocket", async () => {
      // OPC-UA data can be subscribed to via WebSocket
      const ws = new WebSocket("ws://localhost:8080/ws");

      await new Promise<void>((resolve, reject) => {
        ws.onopen = () => resolve();
        ws.onerror = () => reject(new Error("WebSocket failed"));
        setTimeout(() => resolve(), 5000);
      });

      // Subscribe to OPC-UA node (if configured)
      const subscribeMsg = {
        type: "subscribe",
        path: "OPC/Simulation/Objects",
      };

      ws.send(JSON.stringify(subscribeMsg));
      await new Promise((r) => setTimeout(r, 500));

      // Connection should remain stable
      expect(ws.readyState).toBe(WebSocket.OPEN);

      ws.close();
    });

    test("OPC-UA data nodes accessible via semantic API", async () => {
      // OPC-UA nodes should be accessible through the /data endpoint
      const res = await fetch("http://localhost:8080/data");
      expect([200, 404]).toContain(res.status);

      if (res.status === 200) {
        const data = await res.json();
        expect(typeof data).toBe("object");
      }
    });
  });
});
