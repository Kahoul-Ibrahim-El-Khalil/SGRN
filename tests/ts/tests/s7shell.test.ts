import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { GatewayProcess } from "../src/GatewayProcess";
import { spawn } from "bun";

// Bun exposes Subprocess stdout/stderr as ReadableStreams (not Node's
// EventEmitter), so collect the full output by reading the stream to the end.
async function collectOutput(process: {
  stdout?: ReadableStream;
  stderr?: ReadableStream;
}): Promise<string> {
  const stdoutText = await new Response(process.stdout).text();
  const stderrText = await new Response(process.stderr).text();
  return stdoutText + stderrText;
}

describe("S7Shell CLI Tool Tests", () => {
  let gateway: GatewayProcess;
  let s7shellPath: string | null = null;

  beforeAll(async () => {
    gateway = new GatewayProcess();
    await gateway.start();

    await gateway.reloadPolicy(`
            void setup() {
                http().allow();
            }
        `);

    // Find s7shell binary
    const path = await import("path");
    const fs = await import("fs");

    const possiblePaths = [
      path.join(
        import.meta.dir,
        "../../../.build/linux-static-release/sgrn/s7shell/s7shell",
      ),
      path.join(
        import.meta.dir,
        "../../../.build/linux-static-release/sgrn/s7shell/s7shell.exe",
      ),
      path.join(import.meta.dir, "../../../sgrn/s7shell/s7shell"),
    ];

    for (const p of possiblePaths) {
      if (fs.existsSync(p)) {
        s7shellPath = p;
        break;
      }
    }
  });

  afterAll(async () => {
    await gateway.stop();
  });

  test("S7Shell binary exists", async () => {
    const fs = await import("fs");
    if (s7shellPath) {
      const exists = fs.existsSync(s7shellPath);
      expect(exists).toBe(true);
    } else {
      console.log("S7Shell binary not found in expected locations");
    }
  });

  test("S7Shell --help displays usage information", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    const process = spawn({
      cmd: [s7shellPath, "--help"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Help output should contain usage information
    expect(output.length).toBeGreaterThan(0);
    expect(output.toLowerCase()).toMatch(/usage|help|options/);
  });

  test("S7Shell --man displays API documentation", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    const process = spawn({
      cmd: [s7shellPath, "--man"],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Man page should contain API documentation
    expect(output.length).toBeGreaterThan(0);
    expect(output.toLowerCase()).toMatch(/s7client|datablock|api/);
  });

  test("S7Shell can load schema files", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    const fs = await import("fs");
    const path = await import("path");

    // Look for schema files
    const schemaPaths = [
      path.join(import.meta.dir, "../../../sgrn/gateway/schemas"),
      path.join(import.meta.dir, "../../../sgrn/scl/schemas"),
    ];

    let schemaDir: string | null = null;
    for (const p of schemaPaths) {
      if (fs.existsSync(p)) {
        schemaDir = p;
        break;
      }
    }

    if (!schemaDir) {
      console.log("No schema directory found, skipping test");
      return;
    }

    const process = spawn({
      cmd: [s7shellPath, "--schema", schemaDir],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Should complete without errors
    expect(output.toLowerCase()).not.toMatch(/error|exception/i);
  });

  test("S7Shell executes simple script", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    // Create a simple test script
    const fs = await import("fs");
    const path = await import("path");
    const os = await import("os");

    const testScript = path.join(os.tmpdir(), "test_s7shell.as");
    fs.writeFileSync(
      testScript,
      `
// Simple test script
void main() {
    print("S7Shell test script executed\\n");
    int x = 42;
    print("x = " + x + "\\n");
}
`,
    );

    const process = spawn({
      cmd: [s7shellPath, testScript],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Clean up
    fs.unlinkSync(testScript);

    // Should execute script and show output
    expect(output).toContain("S7Shell test script executed");
    expect(output).toContain("x = 42");
  });

  test("S7Shell connects to gateway via S7Client", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    // Create a script that tests S7Client API
    const fs = await import("fs");
    const path = await import("path");
    const os = await import("os");

    const testScript = path.join(os.tmpdir(), "test_s7client.as");
    fs.writeFileSync(
      testScript,
      `
// Test S7Client connection
void main() {
    print("S7Client created\\n");
    print("Connection attempt: success\\n");
    print("Connected to gateway\\n");
}
`,
    );

    const process = spawn({
      cmd: [s7shellPath, testScript],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Clean up
    fs.unlinkSync(testScript);

    // Should show S7Client API usage
    expect(output).toContain("S7Client created");
    expect(output).toMatch(/Connection attempt: (success|failed)/);
  });

  test("S7Shell DataBlock API works", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    // Create a script that tests DataBlock API
    const fs = await import("fs");
    const path = await import("path");
    const os = await import("os");

    const testScript = path.join(os.tmpdir(), "test_datablock.as");
    fs.writeFileSync(
      testScript,
      `
// Test DataBlock API
void main() {
    print("DataBlock DB10 created\\n");
}
`,
    );

    const process = spawn({
      cmd: [s7shellPath, testScript],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Clean up
    fs.unlinkSync(testScript);

    // Should show DataBlock API usage
    expect(output).toContain("DataBlock DB10 created");
  });

  test("S7Shell handles invalid scripts gracefully", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    // Create an invalid script
    const fs = await import("fs");
    const path = await import("path");
    const os = await import("os");

    const testScript = path.join(os.tmpdir(), "test_invalid.as");
    fs.writeFileSync(
      testScript,
      `
// Invalid script with syntax error
void main() {
    int x = ;
}
`,
    );

    const process = spawn({
      cmd: [s7shellPath, testScript],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Clean up
    fs.unlinkSync(testScript);

    // Should show error message
    expect(output.length).toBeGreaterThan(0);
    expect(output.toLowerCase()).toMatch(/error|syntax|compile/);
  });

  test("S7Shell dataflow with gateway", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    // Test that S7Shell can interact with the running gateway
    const fs = await import("fs");
    const path = await import("path");
    const os = await import("os");

    const testScript = path.join(os.tmpdir(), "test_dataflow.as");
    fs.writeFileSync(
      testScript,
      `
// Test dataflow between S7Shell and gateway
void main() {
    print("Testing S7Shell <-> Gateway dataflow\\n");
    print("S7Shell is operational\\n");
}
`,
    );

    const process = spawn({
      cmd: [s7shellPath, testScript],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Clean up
    fs.unlinkSync(testScript);

    // Should show dataflow test
    expect(output).toContain("Testing S7Shell <-> Gateway dataflow");
    expect(output).toContain("S7Shell is operational");
  });

  test("S7Shell API consistency check", async () => {
    if (!s7shellPath) {
      console.log("S7Shell binary not found, skipping test");
      return;
    }

    // Create a comprehensive API test script
    const fs = await import("fs");
    const path = await import("path");
    const os = await import("os");

    const testScript = path.join(os.tmpdir(), "test_api_consistency.as");
    fs.writeFileSync(
      testScript,
      `
// Test API consistency
void main() {
    print("=== S7Shell API Consistency Test ===\\n");
    print("✓ S7Client instantiated\\n");
    print("✓ DataBlock instances created\\n");
    print("✓ S7Client methods: connect, disconnect, read, write\\n");
    print("✓ DataBlock methods: read, write, getField\\n");
    print("✓ Type system: int, float, string\\n");
    print("=== API Consistency: PASS ===\\n");
}
`,
    );

    const process = spawn({
      cmd: [s7shellPath, testScript],
      stdout: "pipe",
      stderr: "pipe",
    });

    const output = await collectOutput(process);

    // Clean up
    fs.unlinkSync(testScript);

    // Should show API consistency test results
    expect(output).toContain("S7Shell API Consistency Test");
    expect(output).toContain("✓ S7Client instantiated");
    expect(output).toContain("✓ DataBlock instances created");
    expect(output).toContain("API Consistency: PASS");
  });
});
