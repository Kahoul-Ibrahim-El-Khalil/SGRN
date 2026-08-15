// security.as — SGRN Gateway Policy Configuration
// ─────────────────────────────────────────────────────────────────────────────
// This file is compiled at gateway boot and on SIGHUP (hot-reload).
// Rules are evaluated in MOST-SPECIFIC-FIRST order. First match wins.
// Deny-by-default: a request with no matching rule is DENIED.
//
// API:
//   s7()      → PolicyBuilder   (IP/CIDR + optional DB filter)
//   http()    → PolicyBuilder   (IP/CIDR + origin + headers)
//   ws()      → PolicyBuilder   (IP/CIDR + origin)
//   modbus()  → PolicyBuilder   (IP/CIDR + DB)
//   opcua()   → PolicyBuilder   (IP/CIDR + session name)
//   eip()     → PolicyBuilder   (IP/CIDR)
//
//   .allowIp("x.x.x.x")         — match exact IPv4
//   .allowCidr("x.x.x.x/n")     — match subnet
//   .allowDb(42)                 — S7/Modbus: restrict to specific DB
//   .allowDb({1, 2, 3})          — restrict to multiple DBs
//   .allowOrigin("http://hmi.local") — HTTP/WS Origin header
//   .requireHeader("X-API-Key") — HTTP: header must be present
//   .allowSession("HMI-1")      — OPC-UA: session display name
//   .allowFieldRead("path.*")   — HTTP/WS/OPC: restrict path read
//   .allowField({"a", "b"})     — HTTP/WS/OPC: multiple paths r/w
//   .denyFieldWrite("path")     — HTTP/WS/OPC: deny path write
//   .allow()                     — ALLOW if all predicates pass
//   .deny()                      — DENY  if all predicates pass
// ─────────────────────────────────────────────────────────────────────────────

void setup() {

    // ── S7 (PLC server) ────────────────────────────────────────────────────
    s7().allow();
    // ── HTTP REST API ──────────────────────────────────────────────────────
    http().allow();

    // ── WebSocket telemetry ────────────────────────────────────────────────
    // Relaxed for simulation: allow all WebSocket traffic.
    ws().allow();

    // ── Modbus TCP ─────────────────────────────────────────────────────────
   // modbus().allow();

    // ── OPC-UA ────────────────────────────────────────────────────────────
    opcua().allow();

    // ── EtherNet/IP ───────────────────────────────────────────────────────
    eip().allow();
}
