
// ─── Environment Setup (formerly env.as) ────────────────────────────────────

const string SCHEMA_PATH = "schema.scl";

const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";
PlcRuntime@ plc = PlcRuntime(SCHEMA_PATH);
GatewaySync@ sync = null;

bool setupEnv(
    const string&in ws_url = GATEWAY_WS_URL
) {
    print("================================================================\n");
    print("  Connecting simulation runtime to gateway at " + ws_url + "\n");
    @sync = GatewaySync(plc);
    sync.useBinary(true);
    sync.publishOnDirty(true);
    bool ok = sync.connect(ws_url);
    if (ok) print("  RuntimeSync started — DB updates will flow over WebSocket.\n"); else print("  RuntimeSync startup failed: " + sync.lastError() + "\n");
    print("================================================================\n");
    return ok;
}

// ─── Runtime Scenario Knobs (env-driven; no source rewriting needed) ───────
// Experiments script the scenario through environment variables instead of
// rewriting this file. s7shell runs this script directly as the soft-PLC and
// can persist the WAL itself via the Persistence binding (PersistenceService
// standalone, no gateway). Defaults reproduce the legacy hourly daemon run.
//
//   SGRN_TICKS_PER_DAY       24 → 1 tick = 1 simulated hour (legacy);
//                            1440 → 1 tick = 1 simulated minute.
//   SGRN_TICK_SLEEP_MS       wall-clock sleep per tick (pacing only).
//   SGRN_SIM_MAX_ITERATIONS  hard stop; <=0 → run until killed (legacy).
//   SGRN_REPORT_EVERY        printReport cadence in ticks.
//   SGRN_FAULT_CODE          selects the fault library entry (9 = FLARE_BLOCKED).
//   SGRN_FAULT_ONSET_HOUR    simulated hour of fault onset (e.g. 723).
//   SGRN_FLARE_RAMP_RATE     bar per simulated hour (flare header press).
//   SGRN_FEED_RAMP_RATE      bar per simulated hour (feed pressure ramp).
//   SGRN_CAPTURE_DIR         when set, WAL persist via Persistence — no gateway.

int envInt(const string &in name, int fallback) {
    string v = getEnv(name);
    int acc = 0;
    uint digits = 0;
    for (uint i = 0; i < v.length(); ++i) {
        uint c = v[i];
        if (c >= 48 && c <= 57) { acc = acc * 10 + int(c - 48); ++digits; }
        else if (digits == 0) return fallback; // junk before first digit
        else break;
    }
    return digits == 0 ? fallback : acc;
}

double envDouble(const string &in name, double fallback) {
    string v = getEnv(name);
    double acc = 0.0;
    double frac = 0.1;
    uint digits = 0;
    bool in_frac = false;
    for (uint i = 0; i < v.length(); ++i) {
        uint c = v[i];
        if (c >= 48 && c <= 57) {
            if (in_frac) { acc += double(c - 48) * frac; frac *= 0.1; }
            else acc = acc * 10.0 + double(c - 48);
            ++digits;
        } else if (c == 46 && !in_frac && digits > 0) { // '.' starts the fraction
            in_frac = true;
        } else if (digits > 0) break;
        else return fallback;
    }
    return digits == 0 ? fallback : acc;
}

// Tick granularity: 24 ticks/day (1 tick = 1 simulated hour, legacy) or
// 1440 ticks/day (1 tick = 1 simulated minute) for high-rate captures.
int TICKS_PER_DAY = envInt("SGRN_TICKS_PER_DAY", 24);
double PERIOD_SCALE = double(TICKS_PER_DAY) / 24.0; // ticks per simulated hour
double PERIOD_RATE = 1.0 / PERIOD_SCALE;            // sim-hours per tick (per-tick drift scale)

int tick_sleep_ms = envInt("SGRN_TICK_SLEEP_MS", 16);
int report_every = envInt("SGRN_REPORT_EVERY", 5);
int max_sim_iterations = envInt("SGRN_SIM_MAX_ITERATIONS", 0); // 0 = run until killed
string sgrn_capture_dir = getEnv("SGRN_CAPTURE_DIR");
bool wal_capture = sgrn_capture_dir.length() > 0;

// Fault-9 (FLARE_BLOCKED) scenario — onset expressed in simulated HOURS so the
// same constant reads identically on hourly or minute tick grids.
double flare_ramp_per_hour = envDouble("SGRN_FLARE_RAMP_RATE", 0.15); // bar/sim-hour
double feed_ramp_per_hour = envDouble("SGRN_FEED_RAMP_RATE", 1.15);   // bar/sim-hour
int flare_onset_hour = envInt("SGRN_FAULT_ONSET_HOUR", 0);
double flare_ramp_rate = flare_ramp_per_hour * PERIOD_RATE; // bar/tick
double feed_ramp_rate = feed_ramp_per_hour * PERIOD_RATE;   // bar/tick
const double FLARE_BASE_PRESS = 1.05; // bar, nominal flare header pressure
int esd_trip_tick = -1;

// ─── Convenience: print every DB ─────────────────────────────────────────────

void printAll() {
    InletSeparation.print();
    AdsorberTowers.print();
    RegenSystem.print();
    SwitchingValves.print();
    OutletQuality.print();
    SafetySystems.print();
    Utilities.print();
}

// ─── Simulation State Variables ──────────────────────────────────────────────

// Feed conditions (base values, augmented with noise)
double base_feed_press = 68.0;
double base_feed_flow = 850.0;
double feed_press = 68.0; // bar
double feed_temp = 32.0; // degC
double feed_flow = 850.0; // e3m3/h
double feed_h2s = 2.0; // ppm
double feed_co2 = 1.8; // %

double scrubber_level = 45.0; // %
double coalescer_dp = 0.15; // bar (increases due to fouling)
double fwko_level = 30.0; // %
double fwko_temp = 31.0; // degC

// Noise generator helper
double randomNoise(double amplitude) {
    // Generate pseudo-random value between -amplitude and +amplitude
    // Since AngelScript might not have rand() mapped, we can use a basic LCG or pseudo-random oscillation based on time
    return 0.0; // We'll implement a simple LCG below
}

uint rand_state = 12345;
double randomNoiseAmp(double amplitude) {
    rand_state = (1103515245 * rand_state + 12345) % 2147483648;
    double frac = double(rand_state) / 2147483648.0;
    return (frac * 2.0 - 1.0) * amplitude;
}

// Tower cycle sequencer — 3 towers, one offline (regen) at a time
const int NUM_TOWERS = 3;
array<int> cycle_state = { 0, 0, 1 }; // 0=Adsorb 1=Depress 2=Heat 3=Cool 4=Repress
array<double> time_in_state = { 0.0, 0.0, 0.0 };
array<double> moisture_pct = { 20.0, 55.0, 5.0 };
array<double> bed_dp = { 0.35, 0.55, 0.20 };
array<uint> cycles_done = { 142, 141, 140 };
int lead_lag_index = 2; // index currently taken offline for regen

// ─── Time-Scaling ────────────────────────────────────────────────────────────
// SIM_TO_REAL: how many real seconds 1 simulation tick represents.
// Derived from the tick grid: TICKS_PER_DAY=24 → 3600 s/tick (1 sim hour);
// TICKS_PER_DAY=1440 → 60 s/tick (1 sim minute). To replay at real speed:
// divide any stored timestamp_s by SIM_TO_REAL.
// To replay at N× speed: divide by (SIM_TO_REAL / N).
const double SIM_TO_REAL = 3600.0 / PERIOD_SCALE; // seconds of real plant time per simulation tick

// Scaled sequencer phase durations (in ticks; 1 tick = 1 simulated hour when
// TICKS_PER_DAY=24; multiplied by PERIOD_SCALE under higher tick rates so the
// plant holds the same simulated schedule).
const double T_DEPRESS = 0.0 * PERIOD_SCALE;
const double T_HEAT = 4.1 * PERIOD_SCALE;
const double T_COOL = 1.9 * PERIOD_SCALE;
const double T_REPRESS = 2.0 * PERIOD_SCALE;

// ─── Synthetic time axis ─────────────────────────────────────────────────────
// All DB timestamps use simulated real time anchored at midnight (00:00:00)
// of the calendar day the simulation starts, NOT the wall clock.
// This keeps the historian unambiguous regardless of when the binary runs.
int tick_count = 0;
int fault_code = envInt("SGRN_FAULT_CODE", 0); // 0=normal, 8=sieve non-absorption, 9=FLARE_BLOCKED
int fault_injection_tick = int(flare_onset_hour * PERIOD_SCALE + 0.5); // onset in sim hours → ticks

// Compute a synthetic DTL representing midnight + tick_count ticks.
// DTL is constructed as a fixed date (2000-01-01 00:00:00) plus the
// elapsed simulated time so that datasets are always self-consistent.
DTL@ simulatedDTL() {
    int minutes_per_tick = int(1440.0 / double(TICKS_PER_DAY) + 0.5);
    int total_minutes = tick_count * minutes_per_tick;
    int day = 1 + total_minutes / 1440;
    int hour = (total_minutes / 60) % 24;
    int minute = total_minutes % 60;
    setPlcTime(2000, 1, day, hour, minute, 0);
    DTL ts;
    return ts;
}

double system_inlet_press = 68.0;
double system_outlet_press = 66.5;

// Regen gas loop
double heater_outlet_temp = 35.0; // degC, ramps to ~290 during Heating
double heater_duty = 0.0;
bool heater_flame_on = false;
double cooler_outlet_temp = 35.0; // ramps down to <50 during Cooling
bool blower_running = false;
double blower_flow = 0.0;
double regen_water_removed = 0.0;

// Outlet quality
double dew_point_c = -105.0;
const double DEW_POINT_SPEC = -100.0;
double dust_filter_dp = 0.08;
double sales_gas_flow = 820.0;

// Safety
bool esd_active = false;
double flare_header_press = 1.05;

// Utilities
double fuel_gas_press = 3.2;
double inst_air_press = 7.0;
bool regen_comp_running = true;

// ─── Main Simulation Loop ───────────────────────────────────────────────────

void main() {
    print("================================================================\n");
    print("  SGRN Gas Processing — Molecular Sieve Dehydration Simulator  \n");
    print("================================================================\n");
    print("  Tick grid          : " + TICKS_PER_DAY + " ticks/day ("
          + int(1440.0 / double(TICKS_PER_DAY) + 0.5) + " min/tick)\n");
    print("  SIM_TO_REAL factor : " + SIM_TO_REAL + " s/tick\n");
    print("  Tick sleep interval: " + tick_sleep_ms + " ms wall time per tick\n");
    print("  Midnight anchor     : 2000-01-01 00:00:00 UTC (epoch 946684800)\n");
    print("  Replay at real speed: divide timestamp_s by " + SIM_TO_REAL + "\n");
    print("================================================================\n");

    // Telemetry egress: gateway publishing path via GatewaySync (deployment-
    // faithful: WS → gateway → hydrated twin → PersistenceService → WAL),
    // OR local WAL capture (Persistence binding — no gateway, no WebSocket)
    // when SGRN_CAPTURE_DIR is set. Local format: "binary" (full-DB image
    // per dirty DB, read straight from PLC memory — authoritative under
    // bare s7shell) or "jsonl" (per-leaf deltas — requires a hydrated
    // gateway twin mirror that bare-script runs lack). SGRN_GATEWAY_WS_URL
    // overrides the gateway endpoint when SGRN_CAPTURE_DIR is unset.
    Persistence@ pers = null;
    if (wal_capture) {
        string sgrn_capture_format = getEnv("SGRN_CAPTURE_FORMAT");
        if (sgrn_capture_format.length() == 0) sgrn_capture_format = "binary";
        print("  WAL capture        : " + sgrn_capture_dir + " (" + sgrn_capture_format + ", changes_with_timestamp)\n");
        @pers = Persistence(plc);
        pers.configure(sgrn_capture_dir, sgrn_capture_format, "changes_with_timestamp");
        pers.start();
    } else {
        string ws_url = getEnv("SGRN_GATEWAY_WS_URL");
        if (ws_url.length() == 0) ws_url = GATEWAY_WS_URL;
        if (!setupEnv(ws_url)) {
            print("ERROR: Failed to set up environment. Aborting simulation.\n");
            return;
        }
    }

    int iteration = 0;
    while (max_sim_iterations <= 0 || iteration < max_sim_iterations) {
        // Use synthetic simulated time — NOT wall clock — so historian
        // timestamps represent real plant hours regardless of run speed.
        DTL@ ts = simulatedDTL();

        // 1. Process physics simulation step
        simulatePhysics();

        // 2. Write states to S7 DataBlocks via properties and the '=' operator
        writeInletSeparation(ts);
        writeAdsorberTowers(ts);
        writeRegenSystem(ts);
        writeSwitchingValves(ts);
        writeOutletQuality(ts);
        writeSafetySystems(ts);
        writeUtilities(ts);

        
        // 4. Graceful stop shortly after ESD latches (tail flushes downstream lag)
        if (esd_trip_tick >= 0 && tick_count > esd_trip_tick + int(PERIOD_SCALE * 3.0 + 0.5)) {
            print("[SIMULATION_COMPLETE] ESD latched — stopping after settle tail\n");
            break;
        }

        iteration++;
        int sleep_ms = tick_sleep_ms;
        if (sleep_ms < 1) sleep_ms = 1;
        sleep(sleep_ms);
    }

    print("[SIMULATION_COMPLETE] Finished at iteration " + iteration + " total ticks " + tick_count + "\n");
    if (pers !is null) {
        pers.flush();
        pers.stop();
        print("[WAL_CLOSED] capture=" + sgrn_capture_dir + " ticks=" + tick_count + "\n");
    } else if (sync !is null) {
        sync.disconnect();
    }
}

// ─── Physical Simulation Logic ───────────────────────────────────────────────

void simulatePhysics() {
    tick_count++;
    bool fault_active = (fault_code > 0) && (tick_count >= fault_injection_tick);

    // ── Tower cycle sequencer ──
    for (int i = 0; i < NUM_TOWERS; i++) {
        time_in_state[i] += 1.0;

        if (i == lead_lag_index) {
            // This tower is off-line, walking through the regen cycle
            if (cycle_state[i] == 1 && time_in_state[i] >= T_DEPRESS) {
                cycle_state[i] = 2; // -> Heating
                time_in_state[i] = 0.0;
            } else if (cycle_state[i] == 2 && time_in_state[i] >= T_HEAT) {
                cycle_state[i] = 3; // -> Cooling
                time_in_state[i] = 0.0;
            } else if (cycle_state[i] == 3 && time_in_state[i] >= T_COOL) {
                cycle_state[i] = 4; // -> Repressurizing
                time_in_state[i] = 0.0;
            } else if (cycle_state[i] == 4 && time_in_state[i] >= T_REPRESS) {
                // Regen complete — bring this tower back online
                cycle_state[i] = 0; // -> Adsorbing
                time_in_state[i] = 0.0;
                moisture_pct[i] = 3.0;
                bed_dp[i] = 0.20;
                cycles_done[i]++;

                // Take the NEXT tower offline to start its regen cycle
                lead_lag_index = (lead_lag_index + 1) % NUM_TOWERS;
                cycle_state[lead_lag_index] = 1; // -> Depressurizing
                time_in_state[lead_lag_index] = 0.0;
            }
        } else {
            // On-stream tower — loading up with moisture (0.05 %/sim-hour)
            moisture_pct[i] += (0.05 + (feed_flow > 850.0 ? 0.02 : 0.0)) * PERIOD_RATE;
            if (fault_active && fault_code == 8) {
                // Molecular sieve non-absorption fault at Hour 4:
                // Active adsorber bed fails to absorb moisture -> rapid humidity breakthrough
                moisture_pct[i] += 18.0 * PERIOD_RATE;
            }
            if (moisture_pct[i] > 95.0) moisture_pct[i] = 95.0;
            bed_dp[i] = 0.20 + (moisture_pct[i] / 100.0) * 0.6 + randomNoiseAmp(0.02);
        }
    }

    // ── Apply random process noise and fault physics ──
    feed_press = base_feed_press + randomNoiseAmp(0.5);
    feed_flow = base_feed_flow + randomNoiseAmp(15.0);
    coalescer_dp += 0.0001 * PERIOD_RATE; // Gradual fouling (0.0001/sim-hour)
    if (coalescer_dp > 0.8) coalescer_dp = 0.8;

    // ── Fault 9 (FLARE_BLOCKED): flare header blocks → header pressure ramps,
    //    relief is ineffective so feed pressure ramps up, downstream lags. ──
    if (fault_code == 9) {
        if (tick_count >= fault_injection_tick) {
            int elapsed = tick_count - fault_injection_tick;
            flare_header_press = FLARE_BASE_PRESS + flare_ramp_rate * double(elapsed) + randomNoiseAmp(0.02);
            feed_press = base_feed_press + feed_ramp_rate * double(elapsed) + randomNoiseAmp(0.5);
            double target_inlet = feed_press - 0.3;
            system_inlet_press += (target_inlet - system_inlet_press) * 0.65;
            double target_outlet = system_inlet_press - 1.5 - (bed_dp[0] + bed_dp[1] + bed_dp[2]) / 3.0;
            system_outlet_press += (target_outlet - system_outlet_press) * 0.65;
            for (int i = 0; i < NUM_TOWERS; i++) {
                if (i != lead_lag_index)
                    bed_dp[i] += (0.003 * PERIOD_RATE) * double(elapsed);
            }
        } else {
            flare_header_press = FLARE_BASE_PRESS + randomNoiseAmp(0.01);
        }
    }

    // ── Regen gas heater / cooler dynamics, driven by whichever tower is regenerating ──
    int regen_state = cycle_state[lead_lag_index];
    if (regen_state == 2) { // Heating
        heater_flame_on = true;
        // PID-like approach to set duty
        double error = 290.0 - heater_outlet_temp;
        heater_duty = 40.0 + error * 1.5 + randomNoiseAmp(2.0);
        if (heater_duty > 100.0) heater_duty = 100.0;
        if (heater_duty < 0.0) heater_duty = 0.0;
        
        heater_outlet_temp += error * 0.2 + randomNoiseAmp(1.0);
        blower_running = true;
        blower_flow = 220.0 + randomNoiseAmp(5.0);
    } else if (regen_state == 3) { // Cooling
        heater_flame_on = false;
        heater_duty = 0.0;
        heater_outlet_temp -= (heater_outlet_temp - feed_temp) * 0.2 + randomNoiseAmp(0.5);
        cooler_outlet_temp -= (cooler_outlet_temp - 35.0) * 0.3 + randomNoiseAmp(0.5);
        blower_running = true;
        blower_flow = 220.0 + randomNoiseAmp(5.0);
        regen_water_removed += 0.8 * PERIOD_RATE;
    } else {
        heater_flame_on = false;
        heater_duty = 0.0;
        blower_running = (regen_state == 1 || regen_state == 4);
        blower_flow = blower_running ? (150.0 + randomNoiseAmp(2.0)) : 0.0;
        cooler_outlet_temp += (feed_temp - cooler_outlet_temp) * 0.1;
    }

    // ── Dew point tracks the worst (highest-moisture) on-stream tower ──
    double worst_moisture = 0.0;
    for (int i = 0; i < NUM_TOWERS; i++) {
        if (i != lead_lag_index && moisture_pct[i] > worst_moisture) {
            worst_moisture = moisture_pct[i];
        }
    }
    dew_point_c = -115.0 + (worst_moisture / 95.0) * 25.0;

    // ── System pressures track feed with small tower-swing losses ──
    system_inlet_press = feed_press - 0.3;
    system_outlet_press = system_inlet_press - 1.5 - (bed_dp[0] + bed_dp[1] + bed_dp[2]) / 3.0;

    // ── ESD trip logic ──
    if (dew_point_c > DEW_POINT_SPEC + 5.0 || feed_press > 90.0 || heater_outlet_temp > 320.0) {
        esd_active = true;
        if (esd_trip_tick < 0) {
            esd_trip_tick = tick_count;
        }
    }
}

// ─── DataBlock Writers ───────────────────────────────────────────────────────

void writeInletSeparation(DTL@ ts) {
    InletSeparation.feed_pressure = float(feed_press);
    InletSeparation.feed_temp = float(feed_temp);
    InletSeparation.feed_flow.rate = float(feed_flow);
    InletSeparation.feed_flow.totalizer += feed_flow / 3600.0;
    InletSeparation.feed_flow.low_flow_alarm = feed_flow < 100.0;
    InletSeparation.feed_flow.sensor_fault = false;
    InletSeparation.feed_h2s_ppm = float(feed_h2s);
    InletSeparation.feed_co2_pct = float(feed_co2);

    InletSeparation.scrubber_level_pct = float(scrubber_level);
    InletSeparation.scrubber_press = float(feed_press - 0.1);
    InletSeparation.scrubber_hi_level_trip = scrubber_level > 90.0;
    InletSeparation.scrubber_liquid_dump.position_pct = 0.0f;
    InletSeparation.scrubber_liquid_dump.command_pct = 0.0f;
    InletSeparation.scrubber_liquid_dump.fault = false;
    InletSeparation.scrubber_liquid_dump.limit_open = false;
    InletSeparation.scrubber_liquid_dump.limit_closed = true;

    InletSeparation.coalescer_dp = float(coalescer_dp);
    InletSeparation.coalescer_bypass = false;
    InletSeparation.coalescer_change_due = coalescer_dp > 0.8;

    InletSeparation.fwko_level_pct = float(fwko_level);
    InletSeparation.fwko_temp = float(fwko_temp);
    InletSeparation.fwko_drain_valve.position_pct = 0.0f;
    InletSeparation.fwko_drain_valve.fault = false;

    InletSeparation.timestamp = ts;
    ;
    InletSeparation.put();
}

void writeAdsorberTowers(DTL@ ts) {
    for (int i = 0; i < NUM_TOWERS; i++) {
        AdsorberTowers.towers[i].cycle_state = uint8(cycle_state[i]);
        AdsorberTowers.towers[i].time_in_state_s = uint(time_in_state[i]);
        AdsorberTowers.towers[i].bed_dp = float(bed_dp[i]);
        AdsorberTowers.towers[i].bed_inlet_temp = float(feed_temp);
        AdsorberTowers.towers[i].bed_outlet_temp = float(feed_temp + 1.5);
        AdsorberTowers.towers[i].moisture_loading = float(moisture_pct[i]);
        AdsorberTowers.towers[i].cycles_completed = cycles_done[i];
        AdsorberTowers.towers[i].breakthrough_alarm = moisture_pct[i] > 90.0 && i != lead_lag_index;
        AdsorberTowers.towers[i].crush_damage_susp = bed_dp[i] > 0.9;

        AdsorberTowers.active_adsorbing[i] = (i != lead_lag_index);
    }

    AdsorberTowers.cycle_time_target_s = 3600;
    AdsorberTowers.lead_lag_index = uint8(lead_lag_index);
    AdsorberTowers.sequence_step = uint16(cycle_state[lead_lag_index]);
    AdsorberTowers.sequence_hold = false;
    AdsorberTowers.sequence_fault = false;

    AdsorberTowers.system_inlet_press = float(system_inlet_press);
    AdsorberTowers.system_outlet_press = float(system_outlet_press);
    AdsorberTowers.total_gas_flow.rate = float(feed_flow);
    AdsorberTowers.total_gas_flow.totalizer += feed_flow / 3600.0;
    AdsorberTowers.total_gas_flow.low_flow_alarm = false;
    AdsorberTowers.total_gas_flow.sensor_fault = false;

    AdsorberTowers.timestamp = ts;
    ;
    AdsorberTowers.put();
}

void writeRegenSystem(DTL@ ts) {
    RegenSystem.heater_outlet_temp = float(heater_outlet_temp);
    RegenSystem.heater_duty_pct = float(heater_duty);
    RegenSystem.heater_fuel_valve.position_pct = float(heater_duty);
    RegenSystem.heater_fuel_valve.command_pct = float(heater_duty);
    RegenSystem.heater_fuel_valve.fault = false;
    RegenSystem.heater_flame_on = heater_flame_on;
    RegenSystem.heater_pilot_on = heater_flame_on;
    RegenSystem.heater_high_temp_trip = heater_outlet_temp > 320.0;

    RegenSystem.heater_temp_pid.setpoint = 290.0;
    RegenSystem.heater_temp_pid.process_value = heater_outlet_temp;
    RegenSystem.heater_temp_pid.output_pct = float(heater_duty);
    RegenSystem.heater_temp_pid.kp = 3.0f;
    RegenSystem.heater_temp_pid.ki = 0.03f;
    RegenSystem.heater_temp_pid.kd = 0.1f;
    RegenSystem.heater_temp_pid.integral_acc = 0.0;
    RegenSystem.heater_temp_pid.enabled = true;
    RegenSystem.heater_temp_pid.auto_mode = true;
    RegenSystem.heater_temp_pid.saturated = false;

    RegenSystem.cooler_inlet_temp = float(heater_outlet_temp);
    RegenSystem.cooler_outlet_temp = float(cooler_outlet_temp);
    RegenSystem.cooler_fan_running = cooler_outlet_temp > 45.0;
    RegenSystem.cooler_fan_speed_pct = cooler_outlet_temp > 45.0 ? 80.0f : 20.0f;

    RegenSystem.blower_running = blower_running;
    RegenSystem.blower_speed = blower_running ? 3550 : 0;
    RegenSystem.blower_flow.rate = float(blower_flow);
    RegenSystem.blower_flow.totalizer += blower_flow / 3600.0;
    RegenSystem.blower_flow.low_flow_alarm = false;
    RegenSystem.blower_flow.sensor_fault = false;
    RegenSystem.blower_surge_alarm = false;
    RegenSystem.blower_motor_temp = blower_running ? 68.0f : 30.0f;

    RegenSystem.regen_ko_level_pct = 25.0f;
    RegenSystem.regen_ko_drain_valve.position_pct = 0.0f;
    RegenSystem.regen_ko_drain_valve.fault = false;
    RegenSystem.regen_water_removed_l = regen_water_removed;

    RegenSystem.timestamp = ts;
    ;
    RegenSystem.put();
}

void writeSwitchingValves(DTL@ ts) {
    for (int i = 0; i < NUM_TOWERS; i++) {
        bool online = (i != lead_lag_index);
        int state = cycle_state[i];

        SwitchingValves.inlet_valves[i].position_pct = online ? 100.0f : 0.0f;
        SwitchingValves.inlet_valves[i].limit_open = online;
        SwitchingValves.inlet_valves[i].limit_closed = !online;
        SwitchingValves.inlet_valves[i].fault = false;

        SwitchingValves.outlet_valves[i].position_pct = online ? 100.0f : 0.0f;
        SwitchingValves.outlet_valves[i].limit_open = online;
        SwitchingValves.outlet_valves[i].limit_closed = !online;
        SwitchingValves.outlet_valves[i].fault = false;

        SwitchingValves.regen_in_valves[i].position_pct = (state == 2) ? 100.0f : 0.0f;
        SwitchingValves.regen_in_valves[i].fault = false;

        SwitchingValves.regen_out_valves[i].position_pct = (state == 2 || state == 3) ? 100.0f : 0.0f;
        SwitchingValves.regen_out_valves[i].fault = false;

        SwitchingValves.depress_valves[i].position_pct = (state == 1) ? 100.0f : 0.0f;
        SwitchingValves.depress_valves[i].fault = false;

        SwitchingValves.repress_valves[i].position_pct = (state == 4) ? 100.0f : 0.0f;
        SwitchingValves.repress_valves[i].fault = false;
    }

    int online_count = 0;
    for (int i = 0; i < NUM_TOWERS; i++) {
        if (i != lead_lag_index) online_count++;
    }
    SwitchingValves.valve_mismatch_alarm = (online_count != NUM_TOWERS - 1);
    SwitchingValves.any_valve_fault = false;

    SwitchingValves.timestamp = ts;
    ;
    SwitchingValves.put();
}

void writeOutletQuality(DTL@ ts) {
    OutletQuality.dew_point = float(dew_point_c);
    OutletQuality.dew_point_spec = float(DEW_POINT_SPEC);
    OutletQuality.dew_point_hi_alarm = dew_point_c > DEW_POINT_SPEC;
    OutletQuality.analyzer_fault = false;
    OutletQuality.analyzer_last_cal = ts;
    ;

    OutletQuality.dust_filter_dp = float(dust_filter_dp);
    OutletQuality.dust_filter_bypass = false;
    OutletQuality.dust_filter_change_due = dust_filter_dp > 0.5;

    OutletQuality.sales_gas_flow.rate = float(sales_gas_flow);
    OutletQuality.sales_gas_flow.totalizer += sales_gas_flow / 3600.0;
    OutletQuality.sales_gas_flow.low_flow_alarm = false;
    OutletQuality.sales_gas_flow.sensor_fault = false;
    OutletQuality.sales_gas_press = float(system_outlet_press);
    OutletQuality.sales_gas_temp = float(feed_temp);
    OutletQuality.heating_value = 38.5f;

    OutletQuality.timestamp = ts;
    ;
    OutletQuality.put();
}

void writeSafetySystems(DTL@ ts) {
    SafetySystems.esd_signal = esd_active;
    SafetySystems.esd_auto = esd_active;
    SafetySystems.esd_reason_code = esd_active ? 4001 : 0;

    SafetySystems.trip_high_press = 90.0f;
    SafetySystems.trip_low_press = 20.0f;
    SafetySystems.trip_high_temp = 320.0f;
    SafetySystems.trip_high_dp = 0.9f;

    for (int i = 0; i < NUM_TOWERS; i++) {
        SafetySystems.relief_valve_lifted[i] = bed_dp[i] > 0.95;
    }
    SafetySystems.flare_header_press = float(flare_header_press);
    SafetySystems.vent_valve.position_pct = esd_active ? 100.0f : 0.0f;
    SafetySystems.vent_valve.fault = false;

    for (int i = 0; i < 8; i++) {
        SafetySystems.gas_detectors[i] = 0.5f;
    }
    for (int i = 0; i < 4; i++) {
        SafetySystems.fire_detectors[i] = false;
    }
    SafetySystems.fg_alarm_active = false;

    SafetySystems.alarms[0].active = esd_active;
    SafetySystems.alarms[0].acknowledged = false;
    SafetySystems.alarms[0].code = 4001;
    SafetySystems.alarms[0].priority = 3;
    SafetySystems.alarms[0].description_id = 20;
    SafetySystems.alarms[0].timestamp_ms = 0;

    for (int i = 1; i < 8; i++) {
        SafetySystems.alarms[i].active = false;
        SafetySystems.alarms[i].acknowledged = false;
        SafetySystems.alarms[i].code = 0;
        SafetySystems.alarms[i].priority = 0;
        SafetySystems.alarms[i].description_id = 0;
        SafetySystems.alarms[i].timestamp_ms = 0;
    }

    SafetySystems.active_alarm_count = esd_active ? 1 : 0;
    SafetySystems.any_critical = esd_active;

    SafetySystems.timestamp = ts;
    ;
    SafetySystems.put();
}

void writeUtilities(DTL@ ts) {
    Utilities.fuel_gas_press = float(fuel_gas_press);
    Utilities.fuel_gas_low_press_trip = fuel_gas_press < 1.5;

    Utilities.inst_air_press = float(inst_air_press);
    Utilities.inst_air_low_alarm = inst_air_press < 5.0;
    Utilities.inst_air_dryer_on = true;

    Utilities.regen_comp_running = regen_comp_running;
    Utilities.regen_comp_speed = regen_comp_running ? 8200 : 0;
    Utilities.regen_comp_disch_press = regen_comp_running ? float(feed_press + 1.0) : 0.0f;
    Utilities.regen_comp_vibration = regen_comp_running ? 2.1f : 0.0f;

    Utilities.ups_on_battery = false;
    Utilities.control_power_ok = true;

    Utilities.timestamp = ts;
    ;
    Utilities.put();
}

