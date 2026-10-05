import {
  packets,
  rate,
  statusText,
  statusDotClass,
  debugLog,
  liveTelemetryValues,
  setWorkerRef,
} from "./telemetryStore";
import { fetchRegistryHeaders, ws } from "@sgrn/gateway";

// Shared telemetry boot for both dashboard variants (gateway App and
// replay ReplayApp). A replay dashboard inherits this — same worker,
// same /ws telemetry — but never imports the docs components.
export async function bootTelemetry(): Promise<void> {
  try {
    const registry = await fetchRegistryHeaders();

    const worker = new Worker(new URL("../worker.ts", import.meta.url), {
      type: "module",
    });
    setWorkerRef(worker);

    const wsUrl = ws("/ws");
    worker.postMessage({
      command: "connect",
      args: { url: wsUrl },
    });

    if (registry.dbs) {
      registry.dbs.forEach((db) => {
        worker.postMessage({
          command: "subscribe",
          args: { path: db.name },
        });
      });
    }

    let lastPacketTime = Date.now();

    worker.onmessage = (e) => {
      const msg = e.data;
      if (msg.type === "status") {
        statusText.set(msg.status);
        if (msg.status === "CONNECTED") {
          statusDotClass.set("connected");
        } else if (msg.status.startsWith("ERROR")) {
          statusDotClass.set("error");
        } else {
          statusDotClass.set("offline");
        }
      } else if (msg.type === "debug") {
        debugLog.update((logs) => [...logs.slice(-99), msg.args]);
      } else if (msg.type === "batch") {
        const now = Date.now();
        const elapsed = now - lastPacketTime;
        lastPacketTime = now;
        if (elapsed > 0) {
          const hz = Math.round(1000 / elapsed);
          rate.set(hz);
        }
        liveTelemetryValues.update((values) => {
          const next = { ...values };
          let count = 0;
          for (const key in msg.updates) {
            next[key] = msg.updates[key];
            count++;
          }
          packets.update((p) => p + count);
          return next;
        });
      }
    };
  } catch (err) {
    console.error("Telemetry initialization failed:", err);
  }
}
