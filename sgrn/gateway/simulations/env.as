const string SCHEMA_PATH = "nuclear_plant.scl";

const string IP = "127.0.0.1";

S7Client@ plc = null;
OpcUaServer@ opc = null;

bool setupEnv(
    const string &in ip   = IP,
    int rack              = 0,
    int slot              = 1
) {
    print("================================================================\n");
    print("  Connecting to PLC at " + ip + " (rack=" + rack + ", slot=" + slot + ")\n");

    @plc = S7Client(ip, rack, slot);
    plc.loadSclSchema(SCHEMA_PATH);

    // Start embedded OPC-UA Server directly on the s7shell memory
    @opc = OpcUaServer(plc.runtime(), 4840);
    if (!opc.start()) {
        print("  WARNING: Could not start OPC-UA server on 4840.\n");
    }

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
    if (db1 !is null) db1.get();
    if (db2 !is null) db2.get();
    if (db3 !is null) db3.get();
    if (db4 !is null) db4.get();
    if (db5 !is null) db5.get();
    if (db6 !is null) db6.get();
    if (db7 !is null) db7.get();
}

// ─── Convenience: print every DB ─────────────────────────────────────────────

void printAll() {
    if (db1 !is null) { print("--- ReactorCore (DB1) ---\n");    db1.print(); }
    if (db2 !is null) { print("--- PrimaryCoolant (DB2) ---\n"); db2.print(); }
    if (db3 !is null) { print("--- SteamGenerator (db3) ---\n"); db3.print(); }
    if (db4 !is null) { print("--- Turbine (db4) ---\n");        db4.print(); }
    if (db5 !is null) { print("--- SafetySystems (db5) ---\n");  db5.print(); }
    if (db6 !is null) { print("--- RadMonitoring (db6) ---\n");  db6.print(); }
    if (db7 !is null) { print("--- WasteProcessing (db7) ---\n"); db7.print(); }
}

// ─── Entry point when run standalone ─────────────────────────────────────────

void testEnv() {
    setupEnv();
    print("Pulling initial snapshot from PLC...\n");
    pullAll();
    print("\nCurrent PLC state:\n");
    printAll();
}
