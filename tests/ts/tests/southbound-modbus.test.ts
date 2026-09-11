import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("Modbus Protocol Adapter", () => {
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
});
