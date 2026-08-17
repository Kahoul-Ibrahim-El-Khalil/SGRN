import { Subprocess, spawn } from "bun";
import { join, dirname, basename } from "path";
import {
  existsSync,
  readFileSync,
  writeFileSync,
  copyFileSync,
  rmSync,
  mkdirSync,
} from "fs";

const GATEWAY_BIN_PATH: string = "";
const S7SHELL_BIN_PATH: string = "";
const DEFAULT_POLICY: string = "";

export class GatewayProcess {
  private gatewayProcess: Subprocess | null = null;
  private simulationProcess: Subprocess | null = null;
  private readonly pfeRoot: string;
  private readonly gatewayPath: string;
  private readonly s7shellPath: string;
  private readonly cwd: string;
  private readonly testConfigPath: string;
  private readonly policyPath: string;
  private readonly stateDir: string;
  private readonly repoSimDir: string;

  /**
   * @param simulationScript - optional path to a .as script to run as the simulation.
   *                           If provided, it will be started after the gateway.
   */
  constructor(private readonly simulationScript?: string) {
    this.pfeRoot = join(import.meta.dir, "../../..");
    this.gatewayPath = join(
      this.pfeRoot,
      ".build/linux-static-release/sgrn/gateway/gateway",
    );
    this.s7shellPath = join(
      this.pfeRoot,
      ".build/linux-static-release/sgrn/s7shell/s7shell",
    );
    this.cwd = join(this.pfeRoot, "sgrn/gateway");

    const simName = this.simulationScript
      ? basename(dirname(this.simulationScript))
      : "gas_processing";
    this.repoSimDir = this.simulationScript
      ? dirname(this.simulationScript)
      : join(this.cwd, "simulations/gas_processing");
    this.stateDir = `/tmp/gateway-state-${simName}`;
    this.testConfigPath = join(this.stateDir, "gateway.json");
    this.policyPath = join(this.stateDir, "security.as");
  }

  public async start() {
    if (this.gatewayProcess) return;

    // Ensure state dir exists
    if (!existsSync(this.stateDir)) {
      mkdirSync(this.stateDir, { recursive: true });
    }

    // Handle policy script
    const repoPolicyPath = join(this.repoSimDir, "security.as");
    if (existsSync(repoPolicyPath)) {
      copyFileSync(repoPolicyPath, this.policyPath);
    } else {
      writeFileSync(this.policyPath, DEFAULT_POLICY);
    }

    // Build test config
    const baseConfig = {
      listen: {
        s7: { port: 8102 },
        opcua: { port: 8480 },
        http: { port: 8080 },
        websocket: { port: 8081 },
      },
      schema: join(this.repoSimDir, "schema.scl"),
      security_script: this.policyPath,
    };
    writeFileSync(this.testConfigPath, JSON.stringify(baseConfig, null, 2));

    // Start gateway
    this.gatewayProcess = spawn({
      cmd: [this.gatewayPath, this.testConfigPath],
      cwd: this.cwd,
      stdout: "pipe",
      stderr: "pipe",
    });

    await this.waitUntilReady();

    // Start simulation if provided
    if (this.simulationScript) {
      this.simulationProcess = spawn({
        cmd: [this.s7shellPath, this.simulationScript],
        cwd: dirname(this.simulationScript),
        stdout: "pipe",
        stderr: "pipe",
      });
      // Wait for simulation to connect (e.g., check a known tag)
      await this.waitForSimulation();
    }
  }

  private async waitUntilReady() {
    for (let i = 0; i < 100; i++) {
      try {
        const res = await fetch("http://localhost:8080/endpoints");
        if (res.ok) return;
      } catch {}
      await Bun.sleep(100);
    }
    throw new Error("Gateway failed to start");
  }

  private async waitForSimulation() {
    // Poll a known DB that the simulation writes to (e.g., DB1 or DB2)
    for (let i = 0; i < 100; i++) {
      try {
        const res = await fetch("http://localhost:8080/data/DB1");
        if (res.ok) {
          const data = await res.json();
          // If data is non-empty, simulation is likely running.
          if (Object.keys(data).length > 0) return;
        }
      } catch {}
      await Bun.sleep(200);
    }
    // Don't throw – allow tests to proceed (some may expect empty data)
  }

  public async stop() {
    if (this.simulationProcess) {
      this.simulationProcess.kill();
      await this.simulationProcess.exited;
      this.simulationProcess = null;
    }
    if (this.gatewayProcess) {
      this.gatewayProcess.kill();
      await this.gatewayProcess.exited;
      this.gatewayProcess = null;
    }

    // Clean up state dir
    if (existsSync(this.stateDir)) {
      rmSync(this.stateDir, { recursive: true, force: true });
    }
  }

  public async reloadPolicy(newPolicy: string) {
    if (!this.gatewayProcess) throw new Error("Gateway not started");
    writeFileSync(this.policyPath, newPolicy);
    this.gatewayProcess.kill("SIGHUP");
    await Bun.sleep(500);
  }
}
