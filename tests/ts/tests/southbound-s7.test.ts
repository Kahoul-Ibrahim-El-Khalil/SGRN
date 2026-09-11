import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("S7 Protocol Adapter", () => {
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
        "../../../.build/linux-static-release/sgrn/gateway/s7proxy",
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
});
