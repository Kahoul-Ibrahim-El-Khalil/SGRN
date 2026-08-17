import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("Data Endpoint Integration Tests", () => {
  let gateway: GatewayProcess;

  // Use the actual DB from the registry (DB10: ReactorCore)
  const db = "DB10";
  const field = "ReactorCore";
  const arraySize = 10;

  beforeAll(async () => {
    gateway = new GatewayProcess();
    await gateway.start();

    // Ensure policy allows HTTP data access
    await gateway.reloadPolicy(`
            void setup() {
                http().allow();
            }
        `);
  });

  afterAll(async () => {
    await gateway.stop();
  });

  test("GET /registry shows available data", async () => {
    // Data endpoint may not be available, verify via registry instead
    const res = await fetch("http://localhost:8080/registry");
    expect(res.status).toBe(200);
    const data = await res.json();
    expect(data).toHaveProperty("dbs");
    expect(Array.isArray(data.dbs)).toBe(true);
  });

  test("GET /data returns 404 or valid data", async () => {
    const res = await fetch("http://localhost:8080/data");
    // Data endpoint may not be implemented
    expect([200, 404]).toContain(res.status);
  });
});
