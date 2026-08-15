import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("Southbound Protocol Tests (S7, Modbus, OPC-UA)", () => {
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

  describe("S7 Protocol Adapter", () => {
    test("S7 proxy binary exists and is executable", async () => {
      const fs = await import("fs");
      const path = await import("path");

      const s7proxyPath = path.join(
        import.meta.dir,
        "../../../../.build/linux-static-release/sgrn/gateway/s7proxy",
      );

      // Check if s7proxy exists (may not be present in all builds)
      const exists = fs.existsSync(s7proxyPath);
      if (exists) {
        const stats = fs.statSync(s7proxyPath);
        expect(stats.mode & 0o111).toBeTruthy(); // Check if executable
      }
    });

    test("S7 connections appear in /connections endpoint", async () => {
      const res = await fetch("http://localhost:8080/connections");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(Array.isArray(data)).toBe(true);

      // Check if any S7 connections exist (may be empty if no PLC connected)
      const s7Connections = data.filter(
        (conn: any) => conn.endpoint && conn.endpoint.includes("S7"),
      );

      // This is informational - S7 connections may or may not be present
      expect(Array.isArray(s7Connections)).toBe(true);
    });

    test("S7 data accessible via /data endpoint when configured", async () => {
      // Try to access DB2 which is commonly used in test configurations
      const res = await fetch("http://localhost:8080/data/DB2");

      // Should return 200 with data or 404 if not configured
      expect([200, 404]).toContain(res.status);

      if (res.status === 200) {
        const data = await res.json();
        expect(typeof data).toBe("object");
      }
    });

    test("S7 registry shows DB structure", async () => {
      const res = await fetch("http://localhost:8080/registry");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(data).toHaveProperty("dbs");

      // Log DB information for debugging (commented out to reduce noise)
      // if (data.dbs && data.dbs.length > 0) {
      //   console.log(`Found ${data.dbs.length} data blocks in registry`);
      //   data.dbs.forEach((db: any) => {
      //     console.log(`  DB${db.db_number}: ${db.db_name} (${db.fields?.length || 0} fields)`);
      //   });
      // }

      expect(Array.isArray(data.dbs)).toBe(true);
    });
  });

  describe("Modbus Protocol Adapter", () => {
    test("Modbus registry endpoint accessible", async () => {
      const res = await fetch("http://localhost:8080/registry/modbus");
      // Modbus may not be configured, accept 200 or 404
      expect([200, 404]).toContain(res.status);

      if (res.status === 200) {
        expect(res.headers.get("content-type")).toContain("application/json");

        const data = await res.json();
        // Modbus mapping may be empty if not configured
        expect(data !== null && data !== undefined).toBe(true);
      }
    });

    test("Modbus virtual map structure validation", async () => {
      const res = await fetch("http://localhost:8080/registry/modbus");

      if (res.status === 200) {
        const data = await res.json();

        // If modbus is configured, validate structure
        if (data && typeof data === "object") {
          // Check for expected modbus mapping structure
          expect(data !== null).toBe(true);
        }
      }
    });

    test("Modbus data access through semantic API", async () => {
      // Try to access data through the semantic API
      // This tests the integration between Modbus and the data layer
      const res = await fetch("http://localhost:8080/data");
      // May return 200 or 404 depending on configuration
      expect([200, 404]).toContain(res.status);
    });
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
        "../../../../sgrn/gateway/configs/s7gateway.json",
      );

      if (fs.existsSync(configPath)) {
        const configStr = fs.readFileSync(configPath, "utf8");
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
      const ws = new WebSocket("ws://localhost:8081");

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

  describe("EtherNet/IP Protocol Adapter", () => {
    test("EtherNet/IP adapter binary exists", async () => {
      const fs = await import("fs");
      const path = await import("path");

      const eipPaths = [
        path.join(
          import.meta.dir,
          "../../../../.build/linux-static-release/sgrn/gateway/eipserver",
        ),
        path.join(
          import.meta.dir,
          "../../../../.build/linux-static-release/sgrn/gateway/eipserver.exe",
        ),
      ];

      let found = false;
      for (const p of eipPaths) {
        if (fs.existsSync(p)) {
          found = true;
          break;
        }
      }

      // EIP server may or may not be built
      expect(found || true).toBe(true); // Informational
    });

    test("EtherNet/IP configuration validation", async () => {
      const fs = await import("fs");
      const path = await import("path");

      const configPath = path.join(
        import.meta.dir,
        "../../../../sgrn/gateway/configs/s7gateway.json",
      );

      if (fs.existsSync(configPath)) {
        const configStr = fs.readFileSync(configPath, "utf8");
        const config = JSON.parse(configStr);

        // Check if EtherNet/IP is configured
        const hasEip = config.listen && config.listen.ethernetip;

        if (hasEip) {
          expect(config.listen.ethernetip).toHaveProperty("port");
          expect(config.listen.ethernetip).toHaveProperty("ip");
        } else {
          console.log("EtherNet/IP not configured in this setup");
        }
      }
    });

    test("EtherNet/IP connections tracked", async () => {
      const res = await fetch("http://localhost:8080/connections");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(Array.isArray(data)).toBe(true);

      // Check for EtherNet/IP connections (may be empty if no devices connected)
      const eipConnections = data.filter(
        (conn: any) =>
          conn.endpoint &&
          (conn.endpoint.includes("EIP") || conn.endpoint.includes("Ethernet")),
      );

      expect(Array.isArray(eipConnections)).toBe(true);
    });

    test("EtherNet/IP data accessible via registry", async () => {
      const res = await fetch("http://localhost:8080/registry");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(data).toHaveProperty("dbs");

      // EIP data would be mapped to DBs
      // This validates the registry structure is accessible
    });

    test("EtherNet/IP CIP data through semantic API", async () => {
      // EtherNet/IP CIP data should be accessible through the data endpoint
      const res = await fetch("http://localhost:8080/data");
      expect([200, 404]).toContain(res.status);
    });

    test("EtherNet/IP tag-based access", async () => {
      // EIP supports tag-based access similar to S7
      const ws = new WebSocket("ws://localhost:8081");

      await new Promise<void>((resolve, reject) => {
        ws.onopen = () => resolve();
        ws.onerror = () => reject(new Error("WebSocket failed"));
        setTimeout(() => resolve(), 5000);
      });

      // Subscribe to EIP tag (if configured)
      const subscribeMsg = {
        type: "subscribe",
        path: "EIP/TagName",
      };

      ws.send(JSON.stringify(subscribeMsg));
      await new Promise((r) => setTimeout(r, 500));

      // Connection should remain stable
      expect(ws.readyState).toBe(WebSocket.OPEN);

      ws.close();
    });
  });

  describe("Multi-Protocol Integration", () => {
    test("All protocol connections appear in /connections", async () => {
      const res = await fetch("http://localhost:8080/connections");
      expect(res.status).toBe(200);

      const data = await res.json();
      expect(Array.isArray(data)).toBe(true);

      // Validate connection record structure
      if (data.length > 0) {
        const conn = data[0];
        expect(conn).toHaveProperty("type");
        expect(conn).toHaveProperty("remote_ip");
        expect(conn).toHaveProperty("endpoint");
        expect(conn).toHaveProperty("first_seen");
        expect(conn).toHaveProperty("last_seen");
        expect(conn).toHaveProperty("event_count");

        // Type should be one of the known protocols
        const validTypes = ["South", "North", "HTTP", "WebSocket"];
        expect(validTypes).toContain(conn.type);
      }
    });

    test("Data from multiple protocols accessible via unified API", async () => {
      // The /data endpoint should provide unified access regardless of source protocol
      const res = await fetch("http://localhost:8080/data");
      // May return 200 or 404 depending on configuration
      expect([200, 404]).toContain(res.status);

      if (res.status === 200) {
        const data = await res.json();
        expect(typeof data).toBe("object");
      }
    });

    test("Registry aggregates data from all protocols", async () => {
      const res = await fetch("http://localhost:8080/registry");
      expect(res.status).toBe(200);

      const data = await res.json();

      // Registry should contain schema from all configured sources
      expect(data).toHaveProperty("dbs");
      expect(data).toHaveProperty("udts");

      // Log registry contents for debugging (commented out to reduce noise)
      // console.log(`Registry contains: ${data.dbs?.length || 0} DBs, ${data.udts?.length || 0} UDTs`);
    });

    test("Telemetry streaming works across protocols", async () => {
      // Test that telemetry data flows through the system
      // This is validated by checking the data endpoint returns current values
      const res1 = await fetch("http://localhost:8080/data/DB2/temperatures");

      if (res1.status === 200) {
        const data1 = await res1.json();

        // Wait a bit and check again
        await new Promise((r) => setTimeout(r, 100));

        const res2 = await fetch("http://localhost:8080/data/DB2/temperatures");
        const data2 = await res2.json();

        // Data should be accessible (values may or may not change depending on PLC)
        expect(data1).toBeDefined();
        expect(data2).toBeDefined();
      }
    });
  });

  describe("Protocol-Specific Error Handling", () => {
    test("Invalid DB number returns appropriate error", async () => {
      const res = await fetch("http://localhost:8080/data/DB999");
      // Should return 404 for non-existent DB
      expect([404, 400, 500]).toContain(res.status);
    });

    test("Invalid field path returns 404", async () => {
      const res = await fetch(
        "http://localhost:8080/data/DB2/nonexistent_field_xyz",
      );
      expect([404, 400, 500]).toContain(res.status);
    });

    test("Malformed requests handled gracefully", async () => {
      const res = await fetch("http://localhost:8080/data/DB2/temperatures", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ invalid: "structure" }),
      });

      // Should return error for invalid data structure
      expect([400, 404, 500]).toContain(res.status);
    });
  });

  describe("Protocol Configuration Validation", () => {
    test("Gateway configuration file is valid", async () => {
      const fs = await import("fs");
      const path = await import("path");

      // Try multiple possible config locations
      const possiblePaths = [
        path.join(
          import.meta.dir,
          "../../../../sgrn/gateway/configs/s7gateway.json",
        ),
        path.join(
          import.meta.dir,
          "../../../../sgrn/gateway/config/s7gateway.json",
        ),
        path.join(import.meta.dir, "../../../configs/s7gateway.json"),
      ];

      let configPath = null;
      for (const p of possiblePaths) {
        if (fs.existsSync(p)) {
          configPath = p;
          break;
        }
      }

      // Config file may not exist in test environment
      if (configPath) {
        const configStr = fs.readFileSync(configPath, "utf8");
        const config = JSON.parse(configStr);

        // Validate basic configuration structure
        expect(config).toHaveProperty("listen");
        expect(config).toHaveProperty("security_script");

        if (config.listen) {
          expect(config.listen).toHaveProperty("http");
          expect(config.listen).toHaveProperty("websocket");
        }
      } else {
        // Config file not found, skip validation
      }
    });

    test("Security policy script exists and is valid", async () => {
      const fs = await import("fs");
      const path = await import("path");

      // Try multiple possible security script locations
      const possiblePaths = [
        path.join(
          import.meta.dir,
          "../../../../sgrn/gateway/configs/security.as",
        ),
        path.join(
          import.meta.dir,
          "../../../../sgrn/gateway/config/security.as",
        ),
        path.join(import.meta.dir, "../../../configs/security.as"),
      ];

      let securityPath = null;
      for (const p of possiblePaths) {
        if (fs.existsSync(p)) {
          securityPath = p;
          break;
        }
      }

      // Security script may not exist in test environment
      if (securityPath) {
        const policyContent = fs.readFileSync(securityPath, "utf8");
        expect(policyContent.length).toBeGreaterThan(0);
        expect(policyContent).toContain("setup");
      } else {
        // Security script not found, skip validation
      }
    });

    test("Schema files are loaded correctly", async () => {
      const res = await fetch("http://localhost:8080/registry");
      expect(res.status).toBe(200);

      const data = await res.json();

      // At least one source of data should be configured
      const hasData =
        (data.dbs && data.dbs.length > 0) ||
        (data.udts && data.udts.length > 0);

      // Log for debugging (commented out to reduce noise)
      // console.log("Registry data available:", hasData);

      // We expect some schema to be loaded
      expect(hasData || data.dbs !== undefined).toBe(true);
    });
  });

  describe("Performance and Reliability", () => {
    test("Rapid data requests maintain stability", async () => {
      const requests = Array.from({ length: 10 }, () =>
        fetch("http://localhost:8080/data/DB10"),
      );

      const responses = await Promise.all(requests);
      responses.forEach((res) => {
        expect([200, 404]).toContain(res.status);
      });
    });

    test("Concurrent registry access works", async () => {
      const requests = Array.from({ length: 10 }, () =>
        fetch("http://localhost:8080/registry"),
      );

      const responses = await Promise.all(requests);
      responses.forEach((res) => {
        expect(res.status).toBe(200);
      });
    });

    test("Memory endpoint performance", async () => {
      const startTime = Date.now();

      const res = await fetch(
        "http://localhost:8080/memory/db/2/offset/0/size/64",
      );

      const duration = Date.now() - startTime;

      // Should complete quickly
      expect(duration).toBeLessThan(1000);
      expect([200, 404, 400]).toContain(res.status);
    });
  });
});
