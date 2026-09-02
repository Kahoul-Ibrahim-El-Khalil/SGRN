export interface DbField {
  name: string;
  type: string;
  offset: number;
  bit: number;
  count: number;
  udt_name?: string;
  unit?: string;
  min?: number;
  max?: number;
  enum?: Record<string, string>;
  children?: DbField[];
}

export interface DbSchema {
  number: number;
  name: string;
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

export interface ModbusRegister {
  start?: number;
  count?: number;
  address?: number;
  bit?: number;
  source: string;
  type: string;
  access: string;
}

export interface ModbusMap {
  holding_registers?: ModbusRegister[];
  input_registers?: ModbusRegister[];
  coils?: ModbusRegister[];
  discrete_inputs?: ModbusRegister[];
  [key: string]: unknown;
}

export interface ConnectionInfo {
  type: string;
  ip: string;
  endpoint: string;
  first_seen: number;
  last_seen: number;
  event_count: number;
}

export interface SessionInfo {
  id: number;
  ip: string;
  connect_time: number;
  bytes_sent: number;
  bytes_received: number;
}

export interface LogEntry {
  ts: number;
  level: string;
  msg: string;
}

export interface EndpointInfo {
  path: string;
  method: string;
  description?: string;
}

export interface RegistryTreeNode {
  id: string;
  type: "db" | "field";
  db_num: number;
  db_name: string;
  name: string;
  path: string;
  key: string;
  depth: number;
  is_struct: boolean;
  is_array: boolean;
  field_type: string;
  parent_row_id: string | null;
  count: number;
  unit?: string;
  min?: number;
  max?: number;
  enum_map?: Record<string, string>;
}

export interface RegistryTreeBuildResult {
  nodes: RegistryTreeNode[];
  node_map: Map<string, RegistryTreeNode>;
  db_names: string[];
}

export interface FlatRegistryField {
  name: string;
  type: string;
  udt_name: string;
  offset: string;
  count: number;
  depth: number;
  full_path: string;
  unit?: string;
  min?: number;
  max?: number;
  enum_map?: Record<string, string>;
}

export interface EipProjectionRow {
  db: string;
  instance: number;
  attr: number;
  field: string;
  cipType: string;
}

export interface OpcuaProjectionRow {
  db: string;
  nodeId: string;
  dataType: string;
  valueRank: string;
}

export interface WorkerConnectArgs {
  url: string;
  validKeys?: string[];
}

export type WorkerCommand =
  | { command: "connect"; args: WorkerConnectArgs }
  | { command: "subscribe"; args: { path: string } }
  | { command: "unsubscribe"; args: { path: string } }
  | { command: "clear_subscriptions"; args?: Record<string, never> }
  | { command: "setFlushInterval"; args: { ms: number } };

export type WorkerMessage =
  | { type: "status"; status: string }
  | { type: "debug"; args: { msg: string; color: string } }
  | { type: "batch"; updates: Record<string, { value: unknown; ts: number }> };

export interface DashboardState {
  activeTab: "process" | "registry";
  searchQuery: string;
  registrySearchQuery: string;
  cellMap: Map<string, { val: HTMLElement; sync: HTMLElement; row: HTMLElement }>;
  expandedNodes: Set<string>;
  updateCount: number;
  sessionStart: number | null;
  staticRegistry: RegistryResponse | null;
}

export interface SgrnError {
  error: string;
  scope: ErrorScope;
}

export interface SgrnResult<T> {
  data?: T;
  error?: string;
  scope?: ErrorScope;
}

export enum ErrorScope {
  Network = "Network",
  Runtime = "Runtime",
  Authentication = "Authentication",
  Authorization = "Authorization",
  FileSystem = "FileSystem",
  Unknown = "Unknown",
}

export function isError<T>(result: SgrnResult<T>): result is { error: string; scope: ErrorScope; data?: never } {
  return result.error !== undefined;
}
