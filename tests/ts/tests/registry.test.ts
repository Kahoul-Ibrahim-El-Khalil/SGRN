import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("Registry Endpoint Integration Tests", () => {
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

  test("GET /registry returns valid schema registry JSON", async () => {
    const res = await fetch("http://localhost:8080/registry");
    expect(res.status).toBe(200);

    const data = await res.json();
    expect(data).toHaveProperty("dbs");
    expect(Array.isArray(data.dbs)).toBe(true);
    expect(data).toHaveProperty("udts");
    expect(Array.isArray(data.udts)).toBe(true);
  });
});
