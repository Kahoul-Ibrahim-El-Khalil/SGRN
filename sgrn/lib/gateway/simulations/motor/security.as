// security.as — SGRN Gateway Policy Configuration — Motor / VFD Simulation
// ─────────────────────────────────────────────────────────────────────────────
// Rules are evaluated in MOST-SPECIFIC-FIRST order. First match wins.
// Deny-by-default: a request with no matching rule is DENIED.
// ─────────────────────────────────────────────────────────────────────────────

void setup() {

    // ── S7 (PLC runtime pushing telemetry — simulation.as) ──────────────────
    s7().allow();

    // ── HTTP REST API ──────────────────────────────────────────────────────
    http().allow();

    // ── WebSocket telemetry ────────────────────────────────────────────────
    ws().allow();

    // ── Modbus TCP — disabled for this simulation ──────────────────────────
    // modbus().allow();

    // ── OPC-UA — this IS the control channel: an external client writes
    //    start/stop/speed_sp_pct/accel_time_s directly into DB1
    //    "MotorCommand" as if it were the drive's own control terminals.
    //    Note this rule governs the gateway's own OPC-UA listener; the
    //    actual control surface is the OpcUaServer embedded in
    //    simulation.as on :4840 (a separate process from this gateway).
    opcua().allow();

    // ── EtherNet/IP — disabled, single motor, no I/O network here ──────────
    // eip().allow();
}
