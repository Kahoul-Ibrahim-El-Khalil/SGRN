// security.as — SGRN Gateway Policy — Full-Coverage Type Simulation
// ─────────────────────────────────────────────────────────────────────────
// Blanket-allow all protocols. This simulation is about testing every
// read/write type path, not access-control edge cases.
// ─────────────────────────────────────────────────────────────────────────

void setup() {

    // HTTP REST — used to read back ground-truth values independent of
    // the OPC UA path, and to seed initial field values for write tests.
    http().allow();

    // WebSocket telemetry — useful for live monitoring during manual runs.
    ws().allow();

    // OPC UA — the primary surface under test for all type read/write paths.
    opcua().allow();
}
