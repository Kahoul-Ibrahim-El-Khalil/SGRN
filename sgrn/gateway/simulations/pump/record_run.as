// record_run.as — drives the pump simulation and records to binary WAL
#include "plc_logic.as"

void recordRun(uint32 duration_seconds) {
    print("Starting recorded run for " + duration_seconds + " seconds...\n");
    PlcRuntime@ rt = PlcRuntime("schema.scl");
    Persistence@ pers = Persistence(rt, "pump_run.bin");
    pers.start();
    
    // Simulate steps
    uint32 scans = uint32(duration_seconds * SCAN_HZ);
    for (uint32 i = 0; i < scans; i++) {
        // advance simulation
        sleep(int(1000.0 / SCAN_HZ));
    }
    
    pers.flush();
    pers.stop();
    print("Recording completed.\n");
}
