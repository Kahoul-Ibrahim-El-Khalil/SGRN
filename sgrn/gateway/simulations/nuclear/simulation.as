const string SCHEMA_PATH = "schema.scl";

const string IP = "127.0.0.1";

S7Client@ plc = null;

bool setupEnv(
    const string&in ip = IP,
    int rack = 0,
    int slot = 1
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

// ─── Convenience: print every DB ─────────────────────────────────────────────

void printAll() {
    if (db1 !is null) { print("--- ReactorCore (DB1) ---\n"); db1.print(); }
    if (db2 !is null) { print("--- PrimaryCoolant (DB2) ---\n"); db2.print(); }
    if (db3 !is null) { print("--- SteamGenerator (db3) ---\n"); db3.print(); }
    if (db4 !is null) { print("--- Turbine (db4) ---\n"); db4.print(); }
    if (db5 !is null) { print("--- SafetySystems (db5) ---\n"); db5.print(); }
    if (db6 !is null) { print("--- RadMonitoring (db6) ---\n"); db6.print(); }
    if (db7 !is null) { print("--- WasteProcessing (db7) ---\n"); db7.print(); }
}

// ─── Entry point when run standalone ─────────────────────────────────────────

// ─── Simulation State Variables ──────────────────────────────────────────────

// Reactor Core & control
double reactor_power_mw = 0.0;
double neutron_flux = 1e5;
double reactivity = 0.0;
double period = 999.0;
bool scrammed = false;
double boron_ppm = 1200.0;

// Advanced Physics state
double xenon_poisoning = 0.0; 
double decay_heat_mw = 0.0;
double rcp_speed_rpm = 1485.0;

// Rod positions
double rod_bank_pos = 100.0; // 0..100%
double rod_bank_demand = 100.0;

// Temperatures & coolant
double t_inlet = 292.0; // Core inlet temperature (degC)
double t_outlet = 325.0; // Core outlet temperature (degC)
double przr_press = 155.0; // Pressurizer pressure (bar)
double przr_level = 50.0; // Pressurizer level (%)

// Secondary side
double sg1_press = 70.0;
double sg2_press = 70.0;
double sg1_level = 50.0;
double sg2_level = 50.0;
double turbine_speed = 3000.0;
double gen_power_mw = 0.0;
bool grid_connected = true;

// Spent fuel pool
double sfp_temp = 32.0;
double sfp_level = 12.0; // meters

// Radiation levels
double shield_building_rad = 0.05; // uSv/h
double aux_building_rad = 0.15;
double stack_release = 120.0; // Bq/s

// ─── Main Simulation Loop ───────────────────────────────────────────────────

void main() {
    print("================================================================\n");
    print("  SGRN Nuclear Power Plant Process Simulator (Schema VM)      \n");
    print("================================================================\n");

    // Initialise PLC connection and DB handles via env.as helper.
    // If env.as was already loaded (e.g. for REPL debugging) this simply
    // re-uses or refreshes the existing connection.
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
        writeReactorCore(db1, ts);
        writePrimaryCoolant(db2, ts);
        writeSteamGenerator(db3, ts);
        writeTurbine(db4, ts);
        writeSafetySystems(db5, ts);
        writeRadMonitoring(db6, ts);
        writeWasteProcessing(db7, ts);

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
    // SCRAM condition (high pressure, high temp, or low water levels)
    if (przr_press > 170.0 || t_outlet > 345.0 || sg1_level < 20.0 || sg2_level < 20.0) {
        if (!scrammed) {
            print("!!! REACTOR SCRAM INITIATED !!!\n");
            scrammed = true;
            rod_bank_demand = 0.0;
        }
    }

    // Control rod movement towards demand
    double rod_speed = 1.5; // % per second
    if (rod_bank_pos < rod_bank_demand) {
        rod_bank_pos += rod_speed;
        if (rod_bank_pos > rod_bank_demand) rod_bank_pos = rod_bank_demand;
    } else if (rod_bank_pos > rod_bank_demand) {
        rod_bank_pos -= rod_speed;
        if (rod_bank_pos < rod_bank_demand) rod_bank_pos = rod_bank_demand;
    }

    // Reactivity and Neutronics
    if (scrammed) {
        reactivity = -5000.0 + (rod_bank_pos * 10.0);
        period = -5.0;
        neutron_flux = neutron_flux * 0.4;
        if (neutron_flux < 1e3) neutron_flux = 1e3;
        
        // Fission stops, but decay heat persists
        double fission_power = 0.0;
        decay_heat_mw *= 0.90; // Sped up for demo
        reactor_power_mw = fission_power + decay_heat_mw;
        
        // RCP pumps coast down during a scram/loss of power
        rcp_speed_rpm *= 0.85; // Sped up coast down
    } else {
        // Normal operation: reactivity balanced by rod position, boron, and Xenon
        double rod_reactivity = (rod_bank_pos - 75.0) * 8.0;
        double boron_reactivity = (1200.0 - boron_ppm) * 1.5;
        
        // Xenon poisoning dynamics: builds up over time if power is high, decays if low
        double target_xenon = (reactor_power_mw / 3400.0) * 1500.0; // max 1500 pcm
        xenon_poisoning += (target_xenon - xenon_poisoning) * 0.2; // Sped up
        
        reactivity = rod_reactivity + boron_reactivity - xenon_poisoning;

        if (reactivity > 5.0) {
            period = 100.0 / reactivity;
            reactor_power_mw += 5.0;
            if (reactor_power_mw > 3400.0) reactor_power_mw = 3400.0;
        } else if (reactivity < -5.0) {
            period = 100.0 / reactivity;
            reactor_power_mw -= 5.0;
            if (reactor_power_mw < 0.0) reactor_power_mw = 0.0;
        } else {
            period = 999.0;
        }
        neutron_flux = 1e11 * (reactor_power_mw / 3400.0) + 1e5;
        decay_heat_mw = reactor_power_mw * 0.065; // ~6.5% decay heat fraction
    }

    // Thermal-hydraulics
    double heating_rate = reactor_power_mw / 3400.0;
    double flow_rate = rcp_speed_rpm / 1485.0;
    if (flow_rate < 0.01) flow_rate = 0.01; // prevent divide by zero
    
    t_inlet = 290.0 + (heating_rate * 2.0);
    t_outlet = t_inlet + (heating_rate * 33.0) / flow_rate;

    // Pressurizer dynamics
    if (reactor_power_mw > 0) {
        przr_press = 155.0 + (heating_rate * 2.0) - (1.0 - (przr_level / 50.0)) * 5.0;
    } else {
        przr_press -= 0.5;
        if (przr_press < 1.013) przr_press = 1.013;
    }

    // Secondary side level & pressure
    if (reactor_power_mw > 100.0) {
        sg1_press = 65.0 + (heating_rate * 5.0);
        sg2_press = 65.0 + (heating_rate * 5.0);
        sg1_level -= 0.1;
        sg2_level -= 0.1;
    } else {
        sg1_press -= 0.1;
        sg2_press -= 0.1;
    }

    // Generator & grid sync
    if (reactor_power_mw > 1000.0 && grid_connected) {
        gen_power_mw = (reactor_power_mw * 0.33);
    } else {
        gen_power_mw = 0.0;
    }

    // Spent fuel pool natural cooling
    sfp_temp += 0.05;
    if (sfp_temp > 40.0) {
        sfp_temp -= 0.1;
    }
}

// ─── DataBlock Writers ───────────────────────────────────────────────────────

void writeReactorCore(ReactorCore@ db, DTL@ ts) {
    db.thermal_power = reactor_power_mw;
    db.thermal_power_ratio = float((reactor_power_mw / 3400.0) * 100.0);
    db.neutron_flux = neutron_flux;
    db.reactivity = float(reactivity);
    db.period = float(period);
    db.core_inlet_temp = float(t_inlet);
    db.core_outlet_temp = float(t_outlet);
    db.core_delta_t = float(t_outlet - t_inlet);

    for (int i = 0; i < 4; i++) {
        db.rods[i].position_pct = float(rod_bank_pos);
        db.rods[i].target_pct = float(rod_bank_demand);
        db.rods[i].speed_pct_s = 1.5f;
        db.rods[i].rod_stuck = false;
        db.rods[i].temperature_c = float(320.0 + i);
        db.rods[i].wear_cycles = 1042;
    }

    for (int i = 0; i < 16; i++) {
        db.thermocouples[i].value_c = float(t_outlet - (i * 0.5));
        db.thermocouples[i].high_alarm = false;
        db.thermocouples[i].low_alarm = false;
        db.thermocouples[i].sensor_fault = false;
    }

    db.boron = float(boron_ppm);
    db.reactor_critical = reactor_power_mw > 10.0;
    db.reactor_tripped = scrammed;
    db.write("timestamp", ts);
    db.put();
}

void writePrimaryCoolant(PrimaryCoolant@ db, DTL@ ts) {
    db.hot_leg_temp = float(t_outlet);
    db.cold_leg_temp = float(t_inlet);
    db.przr_pressure = float(przr_press);
    db.przr_level = float(przr_level);

    for (int i = 0; i < 4; i++) {
        db.rcp[i].running = rcp_speed_rpm > 100.0;
        db.rcp[i].speed_rpm = int(rcp_speed_rpm);
        db.rcp[i].flow_m3h = float(24000.0 * (rcp_speed_rpm / 1485.0));
        db.rcp[i].delta_p_bar = float(6.2 * (rcp_speed_rpm / 1485.0));
        db.rcp[i].bearing_temp_c = 54.0f;
        db.rcp[i].seal_leak_lph = 0.02f;
        db.rcp[i].vibration_mm_s = 1.8f;
        db.rcp[i].trip_active = false;
        db.rcp[i].run_hours = 12050;
    }

    db.przr_pid.setpoint = 155.0;
    db.przr_pid.process_value = przr_press;
    db.przr_pid.output_pct = 12.5f;
    db.przr_pid.kp = 5.0f;
    db.przr_pid.ki = 0.05f;
    db.przr_pid.kd = 0.2f;
    db.przr_pid.integral_acc = 0.0;
    db.przr_pid.enabled = true;
    db.przr_pid.auto_mode = true;
    db.przr_pid.saturated = false;

    db.write("timestamp", ts);
    db.put();
}

void writeSteamGenerator(SteamGenerator@ db, DTL@ ts) {
    db.sg_pressure_bar[0] = float(sg1_press);
    db.sg_pressure_bar[1] = float(sg2_press);
    db.sg_level_pct[0] = float(sg1_level);
    db.sg_level_pct[1] = float(sg2_level);
    db.sg_steam_temp_c[0] = 280.0f;
    db.sg_steam_temp_c[1] = 280.0f;

    for (int p = 0; p < 2; p++) {
        db.sg_level_pid[p].setpoint = 50.0;
        db.sg_level_pid[p].process_value = (p == 0) ? sg1_level : sg2_level;
        db.sg_level_pid[p].output_pct = 45.0f;
        db.sg_level_pid[p].kp = 2.0f;
        db.sg_level_pid[p].ki = 0.02f;
        db.sg_level_pid[p].kd = 0.5f;
        db.sg_level_pid[p].integral_acc = 0.0;
        db.sg_level_pid[p].enabled = true;
        db.sg_level_pid[p].auto_mode = true;
        db.sg_level_pid[p].saturated = false;
    }

    db.write("timestamp", ts);
    db.put();
}

void writeTurbine(Turbine@ db, DTL@ ts) {
    db.hp_speed_rpm = float(turbine_speed);
    db.gen_power_mw = gen_power_mw;
    db.gen_voltage_kv = 24.0f;
    db.gen_frequency_hz = 50.002f;
    db.grid_connected = grid_connected;

    db.bearing_temps[0] = 65.2f;
    db.bearing_temps[1] = 64.8f;
    db.bearing_temps[2] = 67.1f;
    db.bearing_temps[3] = 66.9f;
    db.bearing_temps[4] = 58.4f;
    db.bearing_temps[5] = 59.2f;

    db.shaft_vibration_um[0] = 12.0f;
    db.shaft_vibration_um[1] = 11.5f;
    db.shaft_vibration_um[2] = 14.2f;
    db.shaft_vibration_um[3] = 15.0f;

    db.write("timestamp", ts);
    db.put();
}

void writeSafetySystems(SafetySystems@ db, DTL@ ts) {
    db.scram_signal = scrammed;
    db.scram_auto = scrammed;
    db.containment_press_bar = 1.013f;
    db.containment_temp_c = 35.5f;

    db.alarms[0].active = scrammed;
    db.alarms[0].acknowledged = false;
    db.alarms[0].code = 9001;
    db.alarms[0].priority = 3;
    db.alarms[0].description_id = 10;
    db.alarms[0].timestamp_ms = 0;

    for (int i = 1; i < 8; i++) {
        db.alarms[i].active = false;
        db.alarms[i].acknowledged = false;
        db.alarms[i].code = 0;
        db.alarms[i].priority = 0;
        db.alarms[i].description_id = 0;
        db.alarms[i].timestamp_ms = 0;
    }

    db.write("timestamp", ts);
    db.put();
}

void writeRadMonitoring(RadMonitoring@ db, DTL@ ts) {
    db.zone_monitors[0].dose_rate_usvh = 0.12f;
    db.zone_monitors[0].integrated_msv = 12.5;
    db.zone_monitors[0].alarm_high = false;
    db.zone_monitors[0].alarm_very_high = false;
    db.zone_monitors[0].channel_ok = true;

    db.zone_monitors[1].dose_rate_usvh = 0.05f;
    db.zone_monitors[1].integrated_msv = 5.2;
    db.zone_monitors[1].alarm_high = false;
    db.zone_monitors[1].alarm_very_high = false;
    db.zone_monitors[1].channel_ok = true;

    db.zone_monitors[2].dose_rate_usvh = 0.22f;
    db.zone_monitors[2].integrated_msv = 22.1;
    db.zone_monitors[2].alarm_high = false;
    db.zone_monitors[2].alarm_very_high = false;
    db.zone_monitors[2].channel_ok = true;

    db.zone_monitors[3].dose_rate_usvh = 0.08f;
    db.zone_monitors[3].integrated_msv = 8.4;
    db.zone_monitors[3].alarm_high = false;
    db.zone_monitors[3].alarm_very_high = false;
    db.zone_monitors[3].channel_ok = true;

    for (int i = 4; i < 8; i++) {
        db.zone_monitors[i].dose_rate_usvh = 0.02f;
        db.zone_monitors[i].integrated_msv = 1.2;
        db.zone_monitors[i].alarm_high = false;
        db.zone_monitors[i].alarm_very_high = false;
        db.zone_monitors[i].channel_ok = true;
    }

    db.stack_release_bq_s = stack_release;
    db.write("timestamp", ts);
    db.put();
}

void writeWasteProcessing(WasteProcessing@ db, DTL@ ts) {
    db.sfp_temp_c = float(sfp_temp);
    db.sfp_level_m = float(sfp_level);
    db.sfp_boron_ppm = 2400.0f;
    db.write("timestamp", ts);
    db.put();
}

// ─── Reporting ───────────────────────────────────────────────────────────────

void printReport(DTL@ ts, int iter) {
    print("\n================================================================\n");
    print("  Time: " + ts.toString() + " | Iteration: " + iter + "\n");
    print("----------------------------------------------------------------\n");
    print("  Reactor Thermal Power:  " + reactor_power_mw + " MW\n");
    print("  Neutron Flux Level:     " + neutron_flux + " n/cm^2-s\n");
    print("  Reactivity Level:       " + reactivity + " pcm\n");
    print("  Control Rod Bank:       " + rod_bank_pos + " %\n");
    print("  Core Coolant T(out):    " + t_outlet + " C\n");
    print("  Pressurizer Pressure:   " + przr_press + " bar\n");
    print("  Generator Power:        " + gen_power_mw + " MW\n");
    print("  Spent Fuel Pool Temp:   " + sfp_temp + " C\n");
    if (scrammed) {
        print("  Status:                 !!! SCRAMMED !!!\n");
    } else {
        print("  Status:                 POWER OPERATION\n");
    }
    print("================================================================\n\n");
}
