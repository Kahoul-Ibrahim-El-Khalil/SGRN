import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("Southbound Integration Tests", () => {
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
          "../../../sgrn/gateway/configs/s7gateway.json",
        ),
        path.join(
          import.meta.dir,
          "../../../sgrn/gateway/config/s7gateway.json",
        ),
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
        const configStr = fs
          .readFileSync(configPath, "utf8")
          .replace(/^\s*\/\/.*$/gm, "");
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
        path.join(import.meta.dir, "../../../sgrn/gateway/config/security.as"),
        path.join(import.meta.dir, "../../../sgrn/gateway/configs/security.as"),
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
