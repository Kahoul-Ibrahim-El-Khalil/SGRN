// =============================================================================
// gateway_pipeline.as — Complete Gateway Pipeline Script
//
// Orchestrates:
//   1. S7 Soft PLC Server (port 102) for field client connections
//   2. HTTP REST API Server (port 8080) for REST queries / writes
//   3. WebSocket Live Stream Server (port 9001) for telemetry clients
//   4. Persistence WAL Recorder for archival
//   5. Gateway Sync to remote hub
// =============================================================================

void main() {
    print("=================================================================");
    print("      SGRN Industrial Micro-Gateway Pipeline Orchestrator       ");
    print("=================================================================");

    // 1. Core In-Memory Twin
    PlcRuntime@ rt = PlcRuntime("sgrn/lib/gateway/simulations/pipeline/schema.scl");

    // 2. Soft PLC Server — allows S7 clients/HMIs to read DB1 & DB2 over S7 protocol
    S7Server@ s7 = S7Server(rt, "0.0.0.0", 102);
    s7.start();

    // 3. HTTP REST Server — GET/POST http://localhost:8080/data/...
    HttpServer@ http = HttpServer(rt);
    http.start("0.0.0.0", 8080);

    // 4. WebSocket Server — JSON delta stream on port 9001
    WebSocketServer@ ws = WebSocketServer(rt);
    ws.start("0.0.0.0", 9001);

    // 5. Persistence WAL Recorder
    Persistence@ pers = Persistence(rt, "./wal_pipeline/");
    pers.start();

    // 6. Upstream Gateway Sync
    GatewaySync@ sync = GatewaySync(rt);
    sync.subscribeDb(1);
    sync.publishOnDirty(true);

    print("[Pipeline] Micro-Gateway online and ready!");
    print("  S7 Server  : 0.0.0.0:102");
    print("  HTTP REST  : http://localhost:8080/data/");
    print("  WebSocket  : ws://localhost:9001");
    print("  WAL Log    : ./wal_pipeline/");
    print("=================================================================");

    // Soft PLC control loop — simulate periodic state updates
    for (int tick = 0; tick < 300; tick++) {
        rt.set(1, "Valves.FlowRateLPM", "" + (250.0 + (tick % 20)));
        rt.set(1, "Valves.InletOpen", tick % 2 == 0 ? "true" : "false");

        if (tick % 30 == 0) {
            ws.broadcast("{\"event\":\"pipeline_pulse\",\"tick\":" + tick + "}");
            print("[Pipeline] Pulse tick " + tick + " — active clients: " + s7.clientsCount());
        }

        sleep(1000); // 1 second loop cycle
    }

    // Cleanup
    ws.stop();
    http.stop();
    s7.stop();
    pers.stop();
    print("[Pipeline] Gateway shutdown complete.");
}
