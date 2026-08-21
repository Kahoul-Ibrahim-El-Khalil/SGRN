// security.as — SGRN Gateway Policy Configuration — Enum Edge-Case Simulation
// ─────────────────────────────────────────────────────────────────────────────
// Rules are evaluated in MOST-SPECIFIC-FIRST order. First match wins.
// Deny-by-default: a request with no matching rule is DENIED.
// ─────────────────────────────────────────────────────────────────────────────

void setup() {

    // ── HTTP REST API — used by the test script to read back ground-truth
    //    values independent of the OPC-UA path being tested.
    http().allow();

    // ── WebSocket telemetry — not exercised by the enum tests but harmless
    //    to leave open for manual inspection during debugging.
    ws().allow();

    // ── Modbus TCP — disabled, not relevant to enum projection (Modbus has
    //    no native enum concept; enum_map is metadata-only there).
    // modbus().allow();

    // ── OPC-UA — this IS the surface under test. Blanket allow: this
    //    simulation is specifically about exercising the enum read/write/
    //    registration paths, not access-control edge cases (those are
    //    covered separately). No field-level restrictions.
    opcua().allow();

    // ── EtherNet/IP — disabled, not relevant to enum projection.
    // eip().allow();
}
