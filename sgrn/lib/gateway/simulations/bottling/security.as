// security.as — SGRN Gateway Policy Configuration — Bottling Plant
// ─────────────────────────────────────────────────────────────────────────────
// Rules are evaluated in MOST-SPECIFIC-FIRST order. First match wins.
// Deny-by-default: a request with no matching rule is DENIED.
// ─────────────────────────────────────────────────────────────────────────────

void setup() {

    // ── S7 (PLC runtime pushing telemetry — simulation.as) ──────────────────
    s7().allow();

    // ── HTTP REST API (line dashboards / MES integration) ─────────────────
    http().allow();

    // ── WebSocket telemetry (live plant overview screens) ─────────────────
    ws().allow();

    // ── Modbus TCP — disabled for this simulation ──────────────────────────
    // modbus().allow();

    // ── OPC-UA (historian / SCADA bridge; note setpoints are written to the
    //    OpcUaServer embedded in simulation.as on :4840, a separate process
    //    from this gateway — this rule governs the gateway's own listener) ──
    opcua().allow();

    // ── EtherNet/IP — disabled, no rotating equipment on this network segment
    // eip().allow();
}
