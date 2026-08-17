import type { SecurityRule } from "./types";

export function ipToInt(ip: string): number | null {
  const parts = ip.split(".").map(Number);
  if (parts.length !== 4 || parts.some((p) => Number.isNaN(p) || p < 0 || p > 255)) {
    return null;
  }
  return ((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) >>> 0;
}

export function cidrMatch(cidr: string, ip: string): boolean {
  const parsed = ipToInt(ip);
  if (parsed === null) return false;
  const slash = cidr.indexOf("/");
  if (slash === -1) {
    const target = ipToInt(cidr);
    return target !== null && parsed === target;
  }
  const base = ipToInt(cidr.slice(0, slash));
  const bits = Number.parseInt(cidr.slice(slash + 1), 10);
  if (base === null || Number.isNaN(bits)) return false;
  const mask = bits === 0 ? 0 : (~0 << (32 - bits)) >>> 0;
  return (parsed & mask) >>> 0 === (base & mask) >>> 0;
}

export function ruleMatchesIp(rule: SecurityRule, ip: string): boolean {
  if (!rule.cidrs || rule.cidrs.length === 0) {
    return true;
  }
  return rule.cidrs.some((cidr) => cidrMatch(cidr, ip));
}

export function matchSecurityRule(
  rule: SecurityRule,
  ip: string,
  protocol: string,
  db: number | null,
): "ALLOW" | "DENY" | null {
  if (rule.protocol !== protocol) return null;
  if (!ruleMatchesIp(rule, ip)) return null;
  if (!rule.any_db && db !== null && !rule.dbs.includes(db)) {
    return null;
  }
  return rule.action;
}
