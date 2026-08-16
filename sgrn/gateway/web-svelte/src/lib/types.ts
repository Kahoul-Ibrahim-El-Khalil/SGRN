// ─────────────────────────────────────────────────────────────────────────────
// types.ts — Shared TypeScript types for SGRN Gateway web UI
// ─────────────────────────────────────────────────────────────────────────────

// ── Registry API shapes ───────────────────────────────────────────────────────

export interface DbField {
  name: string;
  type: string;
  offset: number;
  bit_index: number;
  count: number;
  udt_name?: string;
  unit?: string;
  min?: number;
  max?: number;
  enum?: Record<string, string>;
  children?: DbField[];
}

export interface DbSchema {
  db_number: number;
  db_name: string;
  size_bytes: number;
  endianness?: string;
  trigger_events?: boolean;
  fields?: DbField[];
}

export interface UdtSchema {
  name: string;
  size_bytes: number;
  fields?: DbField[];
}

export interface SymbolTag {
  name: string;
  address: string;
  type: string;
  remark?: string;
}

export interface RegistryResponse {
  dbs?: DbSchema[];
  udts?: UdtSchema[];
  tags?: SymbolTag[];
}

// ── Worker message protocol — main → worker ───────────────────────────────────

export type WorkerCommand =
  | { command: "connect"; args: { url: string; validKeys?: string[] } }
  | { command: "subscribe"; args: { path: string } }
  | { command: "unsubscribe"; args: { path: string } }
  | { command: "clear_subscriptions"; args?: Record<string, never> }
  | { command: "setFlushInterval"; args: { ms: number } };

// ── Worker message protocol — worker → main ───────────────────────────────────

export type WorkerMessage =
  | { type: "status"; status: string }
  | { type: "debug"; args: { msg: string; color: string } }
  | { type: "batch"; updates: Record<string, { value: unknown; ts: number }> };

// ── Dashboard UI state ────────────────────────────────────────────────────────

export interface CellEntry {
  val: HTMLElement;
  sync: HTMLElement;
  row: HTMLElement;
}

export interface DashboardState {
  activeTab: "process" | "registry";
  searchQuery: string;
  registrySearchQuery: string;
  cellMap: Map<string, CellEntry>;
  expandedNodes: Set<string>;
  updateCount: number;
  sessionStart: number | null;
  staticRegistry: RegistryResponse | null;
}

// ── Security Policy shapes ───────────────────────────────────────────────────

export interface SecurityRule {
  protocol: string;
  action: "ALLOW" | "DENY";
  specificity: number;
  cidrs: string[];
  dbs: number[];
  any_db: boolean;
  origins: string[];
  headers: string[];
  sessions: string[];
}

export interface SecurityPolicyResponse {
  rules: SecurityRule[];
  total: number;
  mode?: string;
}
