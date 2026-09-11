import { writable } from "svelte/store";

export const packets = writable(0);
export const rate = writable(0);
export const statusText = writable("OFFLINE");
export const statusDotClass = writable("offline"); // offline, connected, error
export const debugLog = writable<{ msg: string; color: string }[]>([]);
export const liveTelemetryValues = writable<
  Record<string, { value: unknown; ts: number }>
>({});

let workerRef: { postMessage: (msg: any) => void } | null = null;

export function setWorkerRef(w: any) {
  workerRef = w;
}

export function sendWorkerCommand(command: string, args: any) {
  if (workerRef) {
    workerRef.postMessage({ command, args });
  }
}
