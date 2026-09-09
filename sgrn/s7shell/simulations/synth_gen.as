// =============================================================================
// synth_gen.as — Parameterized Synthetic Run Generator (template)
//
// Rendered + executed by train_synthetic.py, one file per (scenario, seed).
// Placeholders (all substituted by the driver before invoking s7shell):
//   @SCHEMA@       absolute path to schema.scl
//   @SEED@         PRNG seed (int)
//   @DURATION_S@   simulated horizon in seconds (int)
//   @TIMESTEP_MS@  tick resolution in ms (int)
//   @NOISE@        gaussian noise level (double)
//   @FAULT@        fault scenario string, e.g. healthy | bearing_degradation
//   @OUT_DIR@      WAL output directory (absolute)
//
// Data structure  → schema.scl    (SCL type definitions + DB layout)
// Physics model   → HERE          (motorTick/simTick — same model as
//                                  simulation.as so datasets are comparable)
// Engine loop     → SimEngine@    (C++ tick driver — no field knowledge)
//
// IMPORTANT: keep the runtime construction on ONE line in the form
// PlcRuntime@ g_rt = PlcRuntime( path-to-schema.scl ). The s7shell
// pre-scanner only recognises that single-line shape (declaration +
// construction together); a split declaration/assignment falls back to a
// `plc` client variable that does not exist and the injected db_telemetry
// accessors fail to compile.
// (The same applies inside comments: never write a quoted path inside the
// constructor call in a comment — the pre-scanner scans comments too and
// a bogus match prints a scary but harmless parse error.)
// =============================================================================

// Global runtime handle — single-line form so the pre-scanner binds the
// injected `db_telemetry` / `db1` accessors to g_rt.
PlcRuntime@ g_rt = PlcRuntime("@SCHEMA@");
SimEngine@  g_sim;

// Simulation state read inside the tick callback.
string g_fault_scenario;
double g_noise_level;
uint64 g_total_steps;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ─────────────────────────────────────────────────────────────────────────────
// motorTick — writes one UdtMotorState subtree via = operator proxies.
//
// proxy   : typed handle to the motor subtree  (e.g. db_telemetry.Motor1)
// rated_* : rated operating point for this motor
// phase   : unique phase offset (rad) so each motor's load varies differently
// degrade : bearing degradation 0..100 (non-zero under fault scenario)
// ramp    : startup ramp factor 0..1
// line_on : whether the production line is running
// t_s     : simulated time in seconds
// ─────────────────────────────────────────────────────────────────────────────
void motorTick(
    UdtMotorState@ proxy,
    double rated_rpm, double rated_amps, double rated_torque,
    double phase, double degrade, double ramp, bool line_on, double t_s)
{
    const double PI = 3.14159265358979;

    double speed  = 0.0;
    double amps   = 0.0;
    double torque = 0.0;

    if (line_on) {
        double load = 0.75
            + 0.20 * sin(2.0 * PI * t_s / 120.0 + phase)
            + 0.05 * sin(2.0 * PI * t_s /  17.0 + phase * 2.3);

        double boost = (ramp < 1.0) ? 2.5 * (1.0 - ramp) : 0.0;

        speed  = clamp(rated_rpm    * ramp * load + g_sim.nextNormal() * 3.0  * g_noise_level, 0.0, rated_rpm    * 1.1);
        amps   = clamp(rated_amps   * load * ramp + boost + g_sim.nextNormal() * 0.2 * g_noise_level, 0.0, rated_amps   * 1.8);
        torque = clamp(rated_torque * load * ramp + g_sim.nextNormal() * 1.0  * g_noise_level, 0.0, rated_torque * 1.5);
    }

    // Bearing vibration driven by speed + degradation.
    double vib_base  = 0.05 + (speed / (rated_rpm > 0.0 ? rated_rpm : 1.0)) * 0.15;
    double vib_fault = degrade * 0.04;
    double vib_x = clamp(vib_base + vib_fault * cos(2.0 * PI * t_s / 3.7) + g_sim.nextNormal() * 0.005 * g_noise_level, 0.0, 20.0);
    double vib_y = clamp(vib_base + vib_fault * sin(2.0 * PI * t_s / 4.1) + g_sim.nextNormal() * 0.005 * g_noise_level, 0.0, 20.0);

    double friction_heat = (vib_x + vib_y) * 3.5;
    double temp    = clamp(25.0 + amps * 1.5 + friction_heat + g_sim.nextNormal() * 0.3 * g_noise_level, 20.0, 150.0);
    double acoustic = clamp(0.02 + (vib_x + vib_y) * 0.5 + g_sim.nextNormal() * 0.002 * g_noise_level, 0.0, 10.0);
    double health  = clamp(100.0 - degrade, 0.0, 100.0);

    // ── Write via = operator proxies — no string paths, no DB numbers ──────
    proxy.Running  = line_on;
    proxy.Fault    = (degrade > 70.0);
    proxy.SpeedRPM    = float(speed);
    proxy.CurrentAmps = float(amps);
    proxy.TorqueNm    = float(torque);

    proxy.Bearing.VibrationX    = float(vib_x);
    proxy.Bearing.VibrationY    = float(vib_y);
    proxy.Bearing.Temperature   = float(temp);
    proxy.Bearing.AcousticNoise = float(acoustic);
    proxy.Bearing.HealthScore   = float(health);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick callback — registered with SimEngine.onTick(@simTick)
// Signature: void SimTickFn(PlcRuntime@ rt, double t_s, uint64 step_idx)
//
// `db_telemetry` is a global property injected by the pre-scanner.
// ─────────────────────────────────────────────────────────────────────────────
void simTick(PlcRuntime@ rt, double t_s, uint64 step_idx) {
    const double PI = 3.14159265358979;

    bool line_on  = (t_s >= 2.0) && (step_idx < uint64(double(g_total_steps) * 0.95));
    bool sys_fault = (step_idx >= uint64(double(g_total_steps) * 0.95))
                  && (g_fault_scenario == "bearing_degradation");

    double ramp    = clamp((t_s - 2.0) / 5.0, 0.0, 1.0);
    double progress = (g_total_steps > 0) ? double(step_idx) / double(g_total_steps) : 0.0;
    double degrade_m1 = (g_fault_scenario == "bearing_degradation")
                      ? clamp(progress * progress * 85.0, 0.0, 100.0)
                      : 0.0;

    // ── System-level fields ───────────────────────────────────────────────────
    db_telemetry.LineActive  = line_on;
    db_telemetry.SystemFault = sys_fault;
    db_telemetry.TickCount   = int(step_idx);

    // ── Per-motor ticks — pass typed proxy handles directly ───────────────────
    // Motor1: 1450 RPM nominal, bearing_degradation fault injected
    motorTick(db_telemetry.Motor1, 1450.0, 12.5, 80.0, 0.0,        degrade_m1, ramp, line_on, t_s);
    // Motor2: 1460 RPM nominal, phase-shifted load
    motorTick(db_telemetry.Motor2, 1460.0, 10.8, 70.0, PI / 3.0,   0.0,        ramp, line_on, t_s);
    // Pump1:   960 RPM nominal, slower pump cycle
    motorTick(db_telemetry.Pump1,   960.0,  8.2, 55.0, PI / 1.5,   0.0,        ramp, line_on, t_s);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
void main() {
    print("=================================================================");
    print("      s7shell Synthetic Run Generator (train_synthetic.py)       ");
    print("=================================================================");

    // g_rt was constructed at global scope (see top of file) so the
    // pre-scanner could bind db_telemetry to it. Just validate it here.
    if (g_rt is null) {
        print("[SynthGen] ERROR: runtime failed to initialise, aborting.");
        return;
    }

    // 2. Simulation parameters — timing/seed only. Physics are in simTick().
    SimParams@ p = SimParams();
    p.seed        = @SEED@;
    p.duration_s  = @DURATION_S@;
    p.timestep_ms = @TIMESTEP_MS@;
    p.noise_level = @NOISE@;
    p.fault       = "@FAULT@";

    g_noise_level    = p.noise_level;
    g_fault_scenario = p.fault;
    g_total_steps    = uint64(p.duration_s) * 1000 / uint64(p.timestep_ms);

    // 3. Configure persistence — compressed binary WAL.
    string out_dir = "@OUT_DIR@";
    Persistence@ pers = Persistence(g_rt, out_dir);
    pers.configure(out_dir, "binary", "changes_with_timestamp");
    pers.start();

    print("[SynthGen] seed=" + p.seed + " fault=" + p.fault
        + " WAL target: " + pers.outDir() + "state/unsynced/");

    // 4. Engine drives the tick loop; simTick() owns all physics.
    @g_sim = SimEngine(g_rt, p);
    g_sim.onTick(@simTick);

    print("[SynthGen] Generating " + p.duration_s + "s ("
          + g_total_steps + " ticks)...");

    g_sim.run();

    // 5. Finalise archive.
    pers.flush();
    pers.stop();

    print("[SynthGen] Run complete. WAL: " + pers.outDir() + "state/unsynced/");
}
