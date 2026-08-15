// ============================================================================
// env.as — Nuclear Plant Environment Setup
//
// Provides:
//   - Typed global DB handles (ReactorCore@, PrimaryCoolant@, …)
//   - setupEnv()  — connects, loads schema, initialises handles
//   - pullAll()   — reads all DBs from the PLC
//   - printAll()  — dumps all DBs to stdout
//
// Two-file usage (standalone):
//   ./s7shell env.as nuclear_simulation.as
//
// REPL usage:
//   s7> runScript("path/to/env.as")
//   s7> db10.get(); db10.print()
// ============================================================================

// Top-level constant so the pre-scanner can register schema types
// BEFORE the AngelScript compiler sees ReactorCore@, PrimaryCoolant@, etc.
const string SCHEMA_PATH =
    "schemas/nuclear_plant.scl";

const string IP = "127.0.0.1";
// ─── Environment Initialisation ─────────────────────────────────────────────

S7Client@ plc = null;

bool setupEnv(
    const string &in ip   = IP,
    int rack              = 0,
    int slot              = 1
) {
    print("================================================================\n");
    print("  Connecting to PLC at " + ip + " (rack=" + rack + ", slot=" + slot + ")\n");

    @plc = S7Client(ip, rack, slot);
    plc.loadSclSchema(SCHEMA_PATH);

    bool ok = plc.lastOpOk();
    if (ok) {
        print("  Environment ready — all DB handles initialised.\n");
    } else {
        print("  WARNING: PLC connection issue: " + plc.lastError() + "\n");
    }
    print("================================================================\n");
    return ok;
}

// ─── Convenience: pull a snapshot of every DB from the PLC ──────────────────

void pullAll() {
    if (db10 !is null) db10.get();
    if (db11 !is null) db11.get();
    if (db12 !is null) db12.get();
    if (db13 !is null) db13.get();
    if (db14 !is null) db14.get();
    if (db15 !is null) db15.get();
    if (db16 !is null) db16.get();
}

// ─── Convenience: print every DB ─────────────────────────────────────────────

void printAll() {
    if (db10 !is null) { print("--- ReactorCore (DB10) ---\n");    db10.print(); }
    if (db11 !is null) { print("--- PrimaryCoolant (DB11) ---\n"); db11.print(); }
    if (db12 !is null) { print("--- SteamGenerator (DB12) ---\n"); db12.print(); }
    if (db13 !is null) { print("--- Turbine (DB13) ---\n");        db13.print(); }
    if (db14 !is null) { print("--- SafetySystems (DB14) ---\n");  db14.print(); }
    if (db15 !is null) { print("--- RadMonitoring (DB15) ---\n");  db15.print(); }
    if (db16 !is null) { print("--- WasteProcessing (DB16) ---\n"); db16.print(); }
}

// ─── Entry point when run standalone ─────────────────────────────────────────

void testEnv() {
    setupEnv();
    print("Pulling initial snapshot from PLC...\n");
    pullAll();
    print("\nCurrent PLC state:\n");
    printAll();
}
