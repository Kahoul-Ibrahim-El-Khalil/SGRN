// =============================================================================
// generate_dataset.as — Deterministic Synthetic Dataset Generator
//
// Uses SimEngine + Persistence to generate a 1-hour bearing degradation dataset.
// Run with: s7shell sgrn/s7shell/simulations/generate_dataset.as
// =============================================================================

void main() {
    print("[DatasetGen] Initialising simulation workspace...");

    // 1. Initialize PlcRuntime with SCL schema
    PlcRuntime@ rt = PlcRuntime("sgrn/s7shell/simulations/schema.scl");

    // 2. Configure deterministic simulation parameters
    SimParams@ p = SimParams();
    p.seed        = 42197;                    // PRNG seed for exact reproducibility
    p.duration_s  = 3600;                     // 1 hour simulated time horizon
    p.timestep_ms = 100;                      // 100 ms tick resolution (10 Hz)
    p.noise_level = 0.025;                    // 2.5% Gaussian noise injection
    p.fault       = "bearing_degradation";    // Fault scenario

    // 3. Attach Persistence observer for compressed WAL streaming
    string out_dir = "./datasets/bearing_fault/";
    Persistence@ pers = Persistence(rt, out_dir);
    pers.configure(out_dir, "binary", "changes_with_timestamp");
    pers.start();

    print("[DatasetGen] Recording WAL to " + pers.outDir());

    // 4. Run simulation engine (blocking, fast-forwarded via PlcSimClock)
    SimEngine@ sim = SimEngine(rt, p);
    print("[DatasetGen] Running 1-hour simulation (36,000 ticks)...");
    sim.run();

    // 5. Finalise dataset archive
    pers.flush();
    pers.stop();

    print("[DatasetGen] Complete! Compressed WAL written to " + pers.outDir() + "/unsynced/");
}
