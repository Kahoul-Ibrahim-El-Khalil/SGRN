
// ─── Environment Setup (formerly env.as) ────────────────────────────────────

const string SCHEMA_PATH = "schema.scl";

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
    if (db1 !is null) { print("--- InletSeparation (DB1) ---\n"); db1.print(); }
    if (db2 !is null) { print("--- AdsorberTowers (DB2) ---\n");   db2.print(); }
    if (db3 !is null) { print("--- RegenSystem (DB3) ---\n");      db3.print(); }
    if (db4 !is null) { print("--- SwitchingValves (DB4) ---\n");  db4.print(); }
    if (db5 !is null) { print("--- OutletQuality (DB5) ---\n");    db5.print(); }
    if (db6 !is null) { print("--- SafetySystems (DB6) ---\n");    db6.print(); }
    if (db7 !is null) { print("--- Utilities (DB7) ---\n");        db7.print(); }
}

// ─── Simulation State Variables ──────────────────────────────────────────────

// Feed conditions (base values, augmented with noise)
double base_feed_press = 68.0;
double base_feed_flow = 850.0;
double feed_press = 68.0;      // bar
double feed_temp = 32.0;       // degC
double feed_flow = 850.0;      // e3m3/h
double feed_h2s = 2.0;         // ppm
double feed_co2 = 1.8;         // %

double scrubber_level = 45.0;  // %
double coalescer_dp = 0.15;    // bar (increases due to fouling)
double fwko_level = 30.0;      // %
double fwko_temp = 31.0;       // degC

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
array<int> cycle_state      = {0, 0, 1};   // 0=Adsorb 1=Depress 2=Heat 3=Cool 4=Repress
array<double> time_in_state = {0.0, 0.0, 0.0};
array<double> moisture_pct  = {20.0, 55.0, 5.0};
array<double> bed_dp        = {0.35, 0.55, 0.20};
array<uint> cycles_done     = {142, 141, 140};
int lead_lag_index = 2; // index currently taken offline for regen

const double T_DEPRESS = 5.0;
const double T_HEAT    = 15.0;
const double T_COOL    = 10.0;
const double T_REPRESS = 5.0;

double system_inlet_press = 68.0;
double system_outlet_press = 66.5;

// Regen gas loop
double heater_outlet_temp = 35.0;   // degC, ramps to ~290 during Heating
double heater_duty = 0.0;
bool heater_flame_on = false;
double cooler_outlet_temp = 35.0;   // ramps down to <50 during Cooling
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

    // Initialise PLC connection and DB handles via env.as helper.
    if (!setupEnv()) {
        print("ERROR: Failed to set up environment. Aborting simulation.\n");
        return;
    }

    int iteration = 0;
    while (true) {
        DTL@ ts = dtl();

        // 1. Process physics simulation step
        simulatePhysics();

        // 2. Write states to S7 DataBlocks via properties and the '=' operator
        writeInletSeparation(db1, ts);
        writeAdsorberTowers(db2, ts);
        writeRegenSystem(db3, ts);
        writeSwitchingValves(db4, ts);
        writeOutletQuality(db5, ts);
        writeSafetySystems(db6, ts);
        writeUtilities(db7, ts);

        // 3. Periodic report
        if (iteration % 5 == 0) {
            printReport(ts, iteration);
        }

        iteration++;
        sleep(1000);
    }
}

// ─── Physical Simulation Logic ───────────────────────────────────────────────

void simulatePhysics() {
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
            // On-stream tower — loading up with moisture
            moisture_pct[i] += 0.05 + (feed_flow > 850.0 ? 0.02 : 0.0);
            if (moisture_pct[i] > 95.0) moisture_pct[i] = 95.0;
            bed_dp[i] = 0.20 + (moisture_pct[i] / 100.0) * 0.6 + randomNoiseAmp(0.02);
        }
    }

    // ── Apply random process noise ──
    feed_press = base_feed_press + randomNoiseAmp(0.5);
    feed_flow = base_feed_flow + randomNoiseAmp(15.0);
    coalescer_dp += 0.0001; // Gradual fouling
    if (coalescer_dp > 0.8) coalescer_dp = 0.8;

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
        regen_water_removed += 0.8;
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
    }
}

// ─── DataBlock Writers ───────────────────────────────────────────────────────

void writeInletSeparation(InletSeparation@ db, DTL@ ts) {
    db.feed_pressure = float(feed_press);
    db.feed_temp = float(feed_temp);
    db.feed_flow.rate = float(feed_flow);
    db.feed_flow.totalizer += feed_flow / 3600.0;
    db.feed_flow.low_flow_alarm = feed_flow < 100.0;
    db.feed_flow.sensor_fault = false;
    db.feed_h2s_ppm = float(feed_h2s);
    db.feed_co2_pct = float(feed_co2);

    db.scrubber_level_pct = float(scrubber_level);
    db.scrubber_press = float(feed_press - 0.1);
    db.scrubber_hi_level_trip = scrubber_level > 90.0;
    db.scrubber_liquid_dump.position_pct = 0.0f;
    db.scrubber_liquid_dump.command_pct = 0.0f;
    db.scrubber_liquid_dump.fault = false;
    db.scrubber_liquid_dump.limit_open = false;
    db.scrubber_liquid_dump.limit_closed = true;

    db.coalescer_dp = float(coalescer_dp);
    db.coalescer_bypass = false;
    db.coalescer_change_due = coalescer_dp > 0.8;

    db.fwko_level_pct = float(fwko_level);
    db.fwko_temp = float(fwko_temp);
    db.fwko_drain_valve.position_pct = 0.0f;
    db.fwko_drain_valve.fault = false;

    db.write("timestamp", ts);
    db.put();
}

void writeAdsorberTowers(AdsorberTowers@ db, DTL@ ts) {
    for (int i = 0; i < NUM_TOWERS; i++) {
        db.towers[i].cycle_state = uint8(cycle_state[i]);
        db.towers[i].time_in_state_s = uint(time_in_state[i]);
        db.towers[i].bed_dp = float(bed_dp[i]);
        db.towers[i].bed_inlet_temp = float(feed_temp);
        db.towers[i].bed_outlet_temp = float(feed_temp + 1.5);
        db.towers[i].moisture_loading = float(moisture_pct[i]);
        db.towers[i].cycles_completed = cycles_done[i];
        db.towers[i].breakthrough_alarm = moisture_pct[i] > 90.0 && i != lead_lag_index;
        db.towers[i].crush_damage_susp = bed_dp[i] > 0.9;

        db.active_adsorbing[i] = (i != lead_lag_index);
    }

    db.cycle_time_target_s = 3600;
    db.lead_lag_index = uint8(lead_lag_index);
    db.sequence_step = uint16(cycle_state[lead_lag_index]);
    db.sequence_hold = false;
    db.sequence_fault = false;

    db.system_inlet_press = float(system_inlet_press);
    db.system_outlet_press = float(system_outlet_press);
    db.total_gas_flow.rate = float(feed_flow);
    db.total_gas_flow.totalizer += feed_flow / 3600.0;
    db.total_gas_flow.low_flow_alarm = false;
    db.total_gas_flow.sensor_fault = false;

    db.write("timestamp", ts);
    db.put();
}

void writeRegenSystem(RegenSystem@ db, DTL@ ts) {
    db.heater_outlet_temp = float(heater_outlet_temp);
    db.heater_duty_pct = float(heater_duty);
    db.heater_fuel_valve.position_pct = float(heater_duty);
    db.heater_fuel_valve.command_pct = float(heater_duty);
    db.heater_fuel_valve.fault = false;
    db.heater_flame_on = heater_flame_on;
    db.heater_pilot_on = heater_flame_on;
    db.heater_high_temp_trip = heater_outlet_temp > 320.0;

    db.heater_temp_pid.setpoint = 290.0;
    db.heater_temp_pid.process_value = heater_outlet_temp;
    db.heater_temp_pid.output_pct = float(heater_duty);
    db.heater_temp_pid.kp = 3.0f;
    db.heater_temp_pid.ki = 0.03f;
    db.heater_temp_pid.kd = 0.1f;
    db.heater_temp_pid.integral_acc = 0.0;
    db.heater_temp_pid.enabled = true;
    db.heater_temp_pid.auto_mode = true;
    db.heater_temp_pid.saturated = false;

    db.cooler_inlet_temp = float(heater_outlet_temp);
    db.cooler_outlet_temp = float(cooler_outlet_temp);
    db.cooler_fan_running = cooler_outlet_temp > 45.0;
    db.cooler_fan_speed_pct = cooler_outlet_temp > 45.0 ? 80.0f : 20.0f;

    db.blower_running = blower_running;
    db.blower_speed = blower_running ? 3550 : 0;
    db.blower_flow.rate = float(blower_flow);
    db.blower_flow.totalizer += blower_flow / 3600.0;
    db.blower_flow.low_flow_alarm = false;
    db.blower_flow.sensor_fault = false;
    db.blower_surge_alarm = false;
    db.blower_motor_temp = blower_running ? 68.0f : 30.0f;

    db.regen_ko_level_pct = 25.0f;
    db.regen_ko_drain_valve.position_pct = 0.0f;
    db.regen_ko_drain_valve.fault = false;
    db.regen_water_removed_l = regen_water_removed;

    db.write("timestamp", ts);
    db.put();
}

void writeSwitchingValves(SwitchingValves@ db, DTL@ ts) {
    for (int i = 0; i < NUM_TOWERS; i++) {
        bool online = (i != lead_lag_index);
        int state = cycle_state[i];

        db.inlet_valves[i].position_pct = online ? 100.0f : 0.0f;
        db.inlet_valves[i].limit_open = online;
        db.inlet_valves[i].limit_closed = !online;
        db.inlet_valves[i].fault = false;

        db.outlet_valves[i].position_pct = online ? 100.0f : 0.0f;
        db.outlet_valves[i].limit_open = online;
        db.outlet_valves[i].limit_closed = !online;
        db.outlet_valves[i].fault = false;

        db.regen_in_valves[i].position_pct = (state == 2) ? 100.0f : 0.0f;
        db.regen_in_valves[i].fault = false;

        db.regen_out_valves[i].position_pct = (state == 2 || state == 3) ? 100.0f : 0.0f;
        db.regen_out_valves[i].fault = false;

        db.depress_valves[i].position_pct = (state == 1) ? 100.0f : 0.0f;
        db.depress_valves[i].fault = false;

        db.repress_valves[i].position_pct = (state == 4) ? 100.0f : 0.0f;
        db.repress_valves[i].fault = false;
    }

    int online_count = 0;
    for (int i = 0; i < NUM_TOWERS; i++) {
        if (i != lead_lag_index) online_count++;
    }
    db.valve_mismatch_alarm = (online_count != NUM_TOWERS - 1);
    db.any_valve_fault = false;

    db.write("timestamp", ts);
    db.put();
}

void writeOutletQuality(OutletQuality@ db, DTL@ ts) {
    db.dew_point = float(dew_point_c);
    db.dew_point_spec = float(DEW_POINT_SPEC);
    db.dew_point_hi_alarm = dew_point_c > DEW_POINT_SPEC;
    db.analyzer_fault = false;
    db.write("analyzer_last_cal", ts);

    db.dust_filter_dp = float(dust_filter_dp);
    db.dust_filter_bypass = false;
    db.dust_filter_change_due = dust_filter_dp > 0.5;

    db.sales_gas_flow.rate = float(sales_gas_flow);
    db.sales_gas_flow.totalizer += sales_gas_flow / 3600.0;
    db.sales_gas_flow.low_flow_alarm = false;
    db.sales_gas_flow.sensor_fault = false;
    db.sales_gas_press = float(system_outlet_press);
    db.sales_gas_temp = float(feed_temp);
    db.heating_value = 38.5f;

    db.write("timestamp", ts);
    db.put();
}

void writeSafetySystems(SafetySystems@ db, DTL@ ts) {
    db.esd_signal = esd_active;
    db.esd_auto = esd_active;
    db.esd_reason_code = esd_active ? 4001 : 0;

    db.trip_high_press = 90.0f;
    db.trip_low_press = 20.0f;
    db.trip_high_temp = 320.0f;
    db.trip_high_dp = 0.9f;

    for (int i = 0; i < NUM_TOWERS; i++) {
        db.relief_valve_lifted[i] = bed_dp[i] > 0.95;
    }
    db.flare_header_press = float(flare_header_press);
    db.vent_valve.position_pct = esd_active ? 100.0f : 0.0f;
    db.vent_valve.fault = false;

    for (int i = 0; i < 8; i++) {
        db.gas_detectors[i] = 0.5f;
    }
    for (int i = 0; i < 4; i++) {
        db.fire_detectors[i] = false;
    }
    db.fg_alarm_active = false;

    db.alarms[0].active = esd_active;
    db.alarms[0].acknowledged = false;
    db.alarms[0].code = 4001;
    db.alarms[0].priority = 3;
    db.alarms[0].description_id = 20;
    db.alarms[0].timestamp_ms = 0;

    for (int i = 1; i < 8; i++) {
        db.alarms[i].active = false;
        db.alarms[i].acknowledged = false;
        db.alarms[i].code = 0;
        db.alarms[i].priority = 0;
        db.alarms[i].description_id = 0;
        db.alarms[i].timestamp_ms = 0;
    }

    db.active_alarm_count = esd_active ? 1 : 0;
    db.any_critical = esd_active;

    db.write("timestamp", ts);
    db.put();
}

void writeUtilities(Utilities@ db, DTL@ ts) {
    db.fuel_gas_press  = float(fuel_gas_press);
    db.fuel_gas_low_press_trip = fuel_gas_press < 1.5;

    db.inst_air_press = float(inst_air_press);
    db.inst_air_low_alarm = inst_air_press < 5.0;
    db.inst_air_dryer_on = true;

    db.regen_comp_running = regen_comp_running;
    db.regen_comp_speed = regen_comp_running ? 8200 : 0;
    db.regen_comp_disch_press = regen_comp_running ? float(feed_press + 1.0) : 0.0f;
    db.regen_comp_vibration = regen_comp_running ? 2.1f : 0.0f;

    db.ups_on_battery = false;
    db.control_power_ok = true;

    db.write("timestamp", ts);
    db.put();
}

// ─── Reporting ───────────────────────────────────────────────────────────────

void printReport(DTL@ ts, int iter) {
    print("\n================================================================\n");
    print("  Time: " + ts.toString() + " | Iteration: " + iter + "\n");
    print("----------------------------------------------------------------\n");
    print("  Feed Pressure:          " + feed_press + " bar\n");
    print("  Tower Offline (regen):  T" + (lead_lag_index + 1) + " (state=" + cycle_state[lead_lag_index] + ")\n");
    print("  Moisture Loading (T1-3):" + moisture_pct[0] + " / " + moisture_pct[1] + " / " + moisture_pct[2] + " %\n");
    print("  Regen Heater Outlet:    " + heater_outlet_temp + " C\n");
    print("  Regen Cooler Outlet:    " + cooler_outlet_temp + " C\n");
    print("  Dew Point:              " + dew_point_c + " C (spec " + DEW_POINT_SPEC + " C)\n");
    print("  System Outlet Pressure: " + system_outlet_press + " bar\n");
    if (esd_active) {
        print("  Status:                 !!! ESD ACTIVE !!!\n");
    } else {
        print("  Status:                 NORMAL OPERATION\n");
    }
    print("================================================================\n\n");
}
