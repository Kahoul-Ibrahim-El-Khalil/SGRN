import type { DbSchema, ModbusMap, RegistryResponse, SecurityPolicyResponse } from "./types";
import { api } from "./config";

async function fetchJson<T>(t_path: string, t_error_prefix: string): Promise<T> {
  const res = await fetch(api(t_path));
  if (!res.ok) {
    throw new Error(`${t_error_prefix}: ${res.status}`);
  }
  return (await res.json()) as T;
}

export async function fetchRegistryHeaders(): Promise<RegistryResponse> {
  return fetchJson<RegistryResponse>("/registry?headers=true", "Registry headers fetch failed");
}

export async function fetchDbFields(t_db_number: number): Promise<DbSchema | null> {
  const reg = await fetchJson<RegistryResponse>(`/registry?db=${t_db_number}`, `DB${t_db_number} schema fetch failed`);
  return reg.dbs && reg.dbs.length > 0 ? reg.dbs[0] : null;
}

export async function fetchFullRegistry(): Promise<RegistryResponse> {
  return fetchJson<RegistryResponse>("/registry", "Full registry fetch failed");
}

export async function fetchSecurityPolicy(): Promise<SecurityPolicyResponse> {
  return fetchJson<SecurityPolicyResponse>("/api/policy", "Security policy fetch failed");
}

export async function fetchModbusMap(): Promise<ModbusMap> {
  return fetchJson<ModbusMap>("/registry/modbus", "Modbus registry fetch failed");
}
