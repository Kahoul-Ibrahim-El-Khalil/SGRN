// =============================================================================
// demo_virtual_plc.as — Virtual PLC demo
//
// Creates a fully in-process virtual S7 PLC:
//   1. A PlcRuntime owns the shared schema + memory
//   2. An S7Server serves that memory to any connecting S7 client
//   3. The script writes field values directly via rt.set(), which any
//      real S7 client connecting to the server can immediately read.
//
// Run with:  s7shell demo_virtual_plc.as
// Connect a real client to 127.0.0.1:102 to verify.
// =============================================================================

// 1. Boot runtime from SCL schema
PlcRuntime@ rt = PlcRuntime("schema.scl");
print("Runtime schema loaded.\n");

// 2. Start S7Server -- binds the runtime to a real TCP listener
S7Server@ server = S7Server(rt, "0.0.0.0", 102);
if (!server.start()) {
    print("ERROR: Failed to start S7Server.\n");
    return;
}
print("S7Server listening on port 102.\n");

// 3. Write fields into memory -- immediately visible to all S7 clients
rt.set(1, "pump_1.running",  "true");
rt.set(1, "pump_1.setpoint", "42.5");
rt.set(1, "valve_open",      "false");

print("Memory written. DB1 JSON:\n");
print(rt.getJson(1) + "\n");

// 4. Verify with a loopback S7Client (shares the same runtime)
S7Client@ client = S7Client("127.0.0.1", 0, 1, 102, rt);
auto@ db1 = client.db(1);
print("DB1 via S7Client:\n" + db1.toJson() + "\n");

// 5. Simulate a runtime loop -- flip a flag every second
bool valve = false;
for (int i = 0; i < 5; i++) {
    valve = !valve;
    rt.set(1, "valve_open", valve ? "true" : "false");
    print("valve_open = " + rt.get(1, "valve_open") + "\n");
    sleep(1000);
}

server.stop();
print("Server stopped.\n");
