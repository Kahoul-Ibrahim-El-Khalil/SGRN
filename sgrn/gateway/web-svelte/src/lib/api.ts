// ─────────────────────────────────────────────────────────────────────────────
// api.ts — Data fetchers for SGRN Gateway API
// ─────────────────────────────────────────────────────────────────────────────
import type {
  DbSchema,
  RegistryResponse,
  SecurityPolicyResponse,
} from "./types";
import { api } from "./config";

/** Fetches only DB headers (no field details) — fast first load. */
export async function fetchRegistryHeaders(): Promise<RegistryResponse> {
  const res = await fetch(api("/registry?headers=true"));
  if (!res.ok) throw new Error(`Registry headers fetch failed: ${res.status}`);
  return (await res.json()) as RegistryResponse;
}

/** Fetches fields for a single DB by number. Returns null if not found. */
export async function fetchDbFields(
  dbNumber: number,
): Promise<DbSchema | null> {
  const res = await fetch(api(`/registry?db=${dbNumber}`));
  if (!res.ok)
    throw new Error(`DB${dbNumber} schema fetch failed: ${res.status}`);
  const reg = (await res.json()) as RegistryResponse;
  return reg.dbs && reg.dbs.length > 0 ? reg.dbs[0] : null;
}

/** Fetches the full registry (all DBs with fields, all UDTs, all tags). */
export async function fetchFullRegistry(): Promise<RegistryResponse> {
  const res = await fetch(api("/registry"));
  if (!res.ok) {
    throw new Error(`Full registry fetch failed: ${res.status}`);
  }
  return (await res.json()) as RegistryResponse;
}

/** Fetches the live security policy from the policy introspection endpoint. */
export async function fetchSecurityPolicy(): Promise<SecurityPolicyResponse> {
  const res = await fetch(api("/api/policy"));
  if (!res.ok) throw new Error(`Security policy fetch failed: ${res.status}`);
  return (await res.json()) as SecurityPolicyResponse;
}

export async function fetchModbusMap(): Promise<any> {
  const res = await fetch(api("/registry/modbus"));
  if (!res.ok) throw new Error(`Modbus registry fetch failed: ${res.status}`);
  return await res.json();
}
