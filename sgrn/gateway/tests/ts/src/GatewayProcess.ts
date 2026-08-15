import { Subprocess, spawn } from "bun";
import { join } from "path";
import { copyFileSync, writeFileSync, rmSync, readFileSync } from "fs";

export class GatewayProcess {
  private process: Subprocess | null = null;
  private readonly gatewayPath: string;
  private readonly cwd: string;
  private readonly originalSecurityAs: string;
  private readonly backupSecurityAs: string;
  private readonly testConfigPath: string;

  constructor() {
    const pfeRoot = join(import.meta.dir, "../../../../..");
    this.gatewayPath = join(
      pfeRoot,
      ".build/linux-static-release/sgrn/gateway/gateway",
    );
    this.cwd = join(pfeRoot, "sgrn/gateway");
    this.originalSecurityAs = join(this.cwd, "configs/security.as");
    this.backupSecurityAs = join(this.cwd, "configs/security.as.backup");
    this.testConfigPath = join(this.cwd, "config/s7gateway.test.json");
  }

  public async start() {
    if (this.process) return;

    // Backup original policy
    copyFileSync(this.originalSecurityAs, this.backupSecurityAs);

    // Create test config with unprivileged ports
    const baseConfigStr = readFileSync(
      join(this.cwd, "config/s7gateway.json"),
      "utf8",
    );
    const cleanedConfigStr = baseConfigStr.replace(/^\s*\/\/.*$/gm, "");
    const config = JSON.parse(cleanedConfigStr);
    config.listen.s7.port = 8102;
    config.listen.opcua.port = 8480;
    config.listen.http.port = 8080;
    config.listen.websocket.port = 8081;
    // Point security script to the one we modify
    config.security_script = this.originalSecurityAs;

    writeFileSync(this.testConfigPath, JSON.stringify(config, null, 2));

    this.process = spawn({
      cmd: [this.gatewayPath, "config/s7gateway.test.json"],
      cwd: this.cwd,
      stdout: "pipe",
      stderr: "pipe",
    });

    // Wait for gateway to be ready
    await this.waitUntilReady();
  }

  public async stop() {
    if (!this.process) return;

    this.process.kill();
    await this.process.exited;
    this.process = null;

    // Restore original policy and remove test config
    copyFileSync(this.backupSecurityAs, this.originalSecurityAs);
    rmSync(this.backupSecurityAs);
    rmSync(this.testConfigPath);
  }

  public async reloadPolicy(newPolicyStr: string) {
    if (!this.process) throw new Error("Gateway not started");

    writeFileSync(this.originalSecurityAs, newPolicyStr);
    this.process.kill("SIGHUP");

    // Give it a brief moment to recompile the script
    await new Promise((resolve) => setTimeout(resolve, 500));
  }

  private async waitUntilReady() {
    let attempts = 0;
    while (attempts < 100) {
      try {
        const res = await fetch("http://localhost:8080/endpoints");
        if (res.ok) {
          return;
        }
      } catch (e) {
        // Ignore fetch errors while booting
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
      attempts++;
    }
    throw new Error("Gateway failed to start in time");
  }
}
