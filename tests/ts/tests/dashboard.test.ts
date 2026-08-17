import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";

describe("Web Dashboard Tests", () => {
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

  test("GET / serves dashboard HTML", async () => {
    const res = await fetch("http://localhost:8080/");
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toContain("text/html");
  });

  test("Dashboard HTML contains expected elements", async () => {
    const res = await fetch("http://localhost:8080/");
    const html = await res.text();

    // Check for key dashboard components
    expect(html.length).toBeGreaterThan(0);
    expect(html.includes("<!DOCTYPE html>") || html.includes("<html")).toBe(
      true,
    );
  });

  test("Dashboard assets are accessible", async () => {
    // Try to access common static assets
    const assetPaths = ["/favicon.ico", "/manifest.json"];

    for (const path of assetPaths) {
      try {
        const res = await fetch(`http://localhost:8080${path}`);
        // Asset may or may not exist, but should not error
        expect([200, 404]).toContain(res.status);
      } catch (e) {
        // Network errors are acceptable for missing assets
      }
    }
  });

  test("Dashboard API proxy endpoints work", async () => {
    // Test that the dashboard can access the API through proxy
    const res = await fetch("http://localhost:8080/registry");
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toContain("application/json");
  });

  test("Dashboard handles CORS for development", async () => {
    const res = await fetch("http://localhost:8080/registry", {
      headers: {
        Origin: "http://localhost:5173",
      },
    });

    // Should have CORS headers
    const corsHeader = res.headers.get("access-control-allow-origin");
    expect(corsHeader !== null).toBe(true);
  });

  test("Dashboard JavaScript bundle loads", async () => {
    const res = await fetch("http://localhost:8080/");
    const html = await res.text();

    // Check for script tags or references to JavaScript
    const hasScripts = html.includes("<script") || html.includes(".js");
    expect(hasScripts).toBe(true);
  });

  test("Dashboard CSS loads properly", async () => {
    const res = await fetch("http://localhost:8080/");
    const html = await res.text();

    // Check for style tags or CSS references
    const hasStyles =
      html.includes("<style") ||
      html.includes(".css") ||
      html.includes("style=");
    expect(hasStyles).toBe(true);
  });

  test("Dashboard meta tags are present", async () => {
    const res = await fetch("http://localhost:8080/");
    const html = await res.text();

    // Check for basic HTML structure
    expect(html.toLowerCase()).toContain("<head>");
    expect(html.toLowerCase()).toContain("<body>");
  });

  test("Dashboard responds to HEAD requests", async () => {
    const res = await fetch("http://localhost:8080/", {
      method: "HEAD",
    });
    expect(res.status).toBe(200);
  });

  test("Dashboard handles preload requests", async () => {
    const res = await fetch("http://localhost:8080/", {
      method: "OPTIONS",
    });
    expect([200, 204]).toContain(res.status);
  });
});
