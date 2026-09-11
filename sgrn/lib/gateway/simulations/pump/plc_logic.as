// ============================================================================
// plc_logic.as — "MiniPlant" soft PLC scan cycle
//
// This is program #2: it does NOT own the memory. It connects as an S7Client
// to the sgrn_gateway (program #1) at 127.0.0.1:102, the same as any real
// SCADA/HMI would. Every scan cycle it:
//
//   1. pulls Setpoints (whatever program #3 last forced)
//   2. runs the plant physics (valve travel, pump flow, tank level/temp,
//      heater PID, interlocks/alarms)
//   3. writes Tank / Pump / Valves / Heater / Alarms back into the gateway
//
// Run with:   s7shell plc_logic.as
//
// NOTE: the schema is loaded through a top-level `plc.loadSclSchema(...)`
// call using the variable name `plc` — s7shell's pre-scanner detects this
// pattern and auto-injects one global DataBlock@ handle per DB, named after
// the DB's snake_case name: `setpoints`, `tank`, `pump`, `valves`, `heater`,
// `alarms`. That's what lets the rest of this script read/write DB fields
// as plain properties (e.g. tank.level_pct = ...).
// ============================================================================

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_IP = "127.0.0.1";
const uint16 GATEWAY_PORT = 102;

S7Client@ plc = null;

// ─── Plant constants ─────────────────────────────────────────────────────────
const double TANK_CAPACITY_L = 1000.0;
const double MAX_INLET_LPM = 40.0; // pump @ 100% speed, valve @ 100% open
const double MAX_OUTLET_LPM = 35.0; // gravity drain, valve @ 100% open
const double AMBIENT_C = 20.0;
const double HEATER_GAIN_C_S = 0.06; // deg C per second at 100% heater output
const double COOLING_RATE_C_S = 0.01; // deg C per second passive loss to ambient
const double SCAN_HZ = 5.0; // 5 scans/sec
const double DT = 1.0 / SCAN_HZ;

// ─── Setup ────────────────────────────────────────────────────────────────

OpcUaServer@ opc = null;

bool setupPlc() {
    print("================================================================\n");
    print("  MiniPlant PLC — connecting to gateway at " + GATEWAY_IP + ":" + GATEWAY_PORT + "\n");

    @plc = S7Client(GATEWAY_IP, 0, 1, GATEWAY_PORT);
    plc.loadSclSchema(SCHEMA_PATH);

    if (!plc.isConnected()) {
        print("  ERROR: could not connect to gateway: " + plc.lastError() + "\n");
        return false;
    }

    // Start embedded OPC-UA Server directly on the s7shell memory
    @opc = OpcUaServer(plc.runtime(), 4840);
    if (!opc.start()) {
        print("  WARNING: Could not start OPC-UA server on 4840.\n");
    }

    print("  Connected. Scan rate: " + SCAN_HZ + " Hz\n");
    print("================================================================\n");
    return true;
}

// ─── Valve helper: ramp position towards command at travel-time rate ───────

void driveValve(ValveActuator@ v, double dt) {
    if (v.travel_time_s <= 0.0f)
        v.travel_time_s = 5.0f; // guard against div-by-zero on a fresh/forced record

    double rate_per_s = 100.0 / double(v.travel_time_s); // % per second, full stroke
    double step = rate_per_s * dt;

    double pos = double(v.position_pct);
    double cmd = double(v.command_pct);

    if (pos < cmd) {
        pos += step;
        if (pos > cmd) pos = cmd;
    } else if (pos > cmd) {
        pos -= step;
        if (pos < cmd) pos = cmd;
    }

    if (pos < 0.0) pos = 0.0;
    if (pos > 100.0) pos = 100.0;

    v.position_pct = float(pos);
    v.limit_open = (pos >= 99.5);
    v.limit_closed = (pos <= 0.5);
}

// ─── Main scan cycle ─────────────────────────────────────────────────────────

void main() {
    if (!setupPlc())
        return;

    // We DO NOT sync from gateway inside the loop for process variables.
    // Instead, the s7shell memory IS the engine, and OPC-UA clients connect to IT.
    
    // Seed initial state once
    tank.get();
    pump.get();
    valves.get();
    heater.get();

    double pump_speed = 0.0;
    double runtime_s = double(pump.runtime_s);

    int iteration = 0;

    while (true) {
        DTL@ ts = dtl();

        // Sync local variables from the internal shadow memory (so OPC-UA forcing works)
        double level_l = double(tank.volume_l);
        double temp_c = double(tank.temp_c) > 0.0 ? double(tank.temp_c) : AMBIENT_C;
        
        // 1. Pull the latest operator-forced setpoints from Gateway ─────────────────────
        // (Because Setpoints come from outside HMI via Gateway)
        setpoints.get();

        bool e_stop = setpoints.e_stop;
        bool run_cmd = setpoints.pump_run && !e_stop;
        double speed_sp = double(setpoints.pump_speed_sp_pct);

        // Emergency stop overrides every command: pump off, both valves shut
        double inlet_cmd = e_stop ? 0.0 : double(setpoints.inlet_valve_cmd_pct);
        double outlet_cmd = e_stop ? 0.0 : double(setpoints.outlet_valve_cmd_pct);

        // 2. Valve actuators ────────────────────────────────────────────────
        // We do NOT call valves.get() here; we use the local shadow so OPC-UA can force position/faults!
        valves.inlet.command_pct = float(inlet_cmd);
        valves.outlet.command_pct = float(outlet_cmd);
        driveValve(valves.inlet, DT);
        driveValve(valves.outlet, DT);

        // 3. Pump ────────────────────────────────────────────────────────────
        bool pump_fault = pump.fault; // sticky until acked
        bool pump_running = run_cmd && !pump_fault;

        double speed_target_eff = pump_running ? speed_sp : 0.0;
        
        // ── Cavitation Protection ──
        if (pump_running && (level_l / TANK_CAPACITY_L * 100.0) < 5.0 && speed_target_eff > 50.0) {
            pump_fault = true;
            pump.fault = true;
            pump_running = false;
            speed_target_eff = 0.0;
        }
        double speed_rate = 40.0 * DT; // % per second ramp
        if (pump_speed < speed_target_eff) {
            pump_speed += speed_rate;
            if (pump_speed > speed_target_eff) pump_speed = speed_target_eff;
        } else if (pump_speed > speed_target_eff) {
            pump_speed -= speed_rate;
            if (pump_speed < speed_target_eff) pump_speed = speed_target_eff;
        }
        if (pump_running) runtime_s += DT;

        // 4. Tank hydraulics ─────────────────────────────────────────────────
        double inlet_flow_lpm =
            (pump_speed / 100.0) * (double(valves.inlet.position_pct) / 100.0) * MAX_INLET_LPM;

        // Gravity drain: Torricelli's Law (scales with square root of head)
        // Ensure non-negative value for sqrt approximation
        double level_ratio = level_l / TANK_CAPACITY_L;
        if (level_ratio < 0.0) level_ratio = 0.0;
        
        // Approximate sqrt using a fast iterative or pow approach, or simple approximation since it's just a demo.
        // Actually, AngelScript supports math if we add it, but a simple x^0.5 approximation or just x^2 could work.
        // x^0.5 is standard, let's use a simpler heuristic for Torricelli since we don't know if sqrt is registered.
        // We will just use `level_ratio` directly but with an exaggerated head curve to simulate Torricelli without sqrt.
        // Actually, standard AS has no sqrt built-in unless registered. Let's do a piecewise or polynomial curve:
        double head_factor = (level_ratio > 0.0) ? (level_ratio + 0.1) : 0.0; // simple head modifier
        if (head_factor > 1.0) head_factor = 1.0;

        double outlet_flow_lpm =
            (double(valves.outlet.position_pct) / 100.0) * head_factor * MAX_OUTLET_LPM;

        level_l += (inlet_flow_lpm - outlet_flow_lpm) * (DT / 60.0);
        if (level_l < 0.0) level_l = 0.0;
        if (level_l > TANK_CAPACITY_L) level_l = TANK_CAPACITY_L;

        double level_pct = (level_l / TANK_CAPACITY_L) * 100.0;

        // Safety interlock: overfill forces outlet open + trips the pump
        bool overfill = level_pct >= 99.0;
        if (overfill) {
            valves.outlet.command_pct = 100.0f;
            pump_running = false;
            pump_speed = 0.0;
        }

        // 5. Heater PID (simple P loop driving a first-order thermal lag) ────
        // We do NOT call heater.get() here; we use the local shadow so OPC-UA can force PID states!
        heater.loop.setpoint = double(setpoints.heater_setpoint_c);
        heater.loop.enabled = setpoints.heater_enable && !e_stop;
        heater.loop.process_value = temp_c;

        double heater_output = 0.0;
        if (heater.loop.enabled) {
            double error = double(heater.loop.setpoint) - temp_c;
            heater_output = error * 15.0; // Kp
            if (heater_output < 0.0) heater_output = 0.0;
            if (heater_output > 100.0) heater_output = 100.0;
        }
        heater.loop.output_pct = float(heater_output);
        heater.loop.saturated = (heater_output >= 100.0);

        temp_c += (heater_output / 100.0) * HEATER_GAIN_C_S * DT;
        temp_c -= (temp_c - AMBIENT_C) * COOLING_RATE_C_S * DT;
        if (temp_c < AMBIENT_C) temp_c = AMBIENT_C;

        // 6. Alarms ──────────────────────────────────────────────────────────
        bool high_level = level_pct >= 90.0;
        bool low_level = level_pct <= 5.0;
        int active_count = 0;
        if (high_level) active_count++;
        if (low_level) active_count++;
        if (pump_fault) active_count++;
        if (e_stop) active_count++;

        // 7. Publish everything back to the gateway ─────────────────────────
        tank.level_pct = float(level_pct);
        tank.volume_l = float(level_l);
        tank.temp_c = float(temp_c);
        tank.inlet_flow_lpm = float(inlet_flow_lpm);
        tank.outlet_flow_lpm = float(outlet_flow_lpm);
        tank.high_level_alarm = high_level;
        tank.low_level_alarm = low_level;
        tank.overfill_trip = overfill;
        tank.timestamp = ts;
        tank.put();

        pump.running = pump_running;
        pump.speed_pct = float(pump_speed);
        pump.fault = pump_fault;
        pump.runtime_s = runtime_s;
        pump.timestamp = ts;
        pump.put();

        valves.timestamp = ts;
        valves.put();

        heater.timestamp = ts;
        heater.put();

        alarms.any_active = active_count > 0;
        alarms.active_count = uint16(active_count);
        alarms.e_stop_active = e_stop;
        alarms.high_level = high_level;
        alarms.low_level = low_level;
        alarms.pump_fault = pump_fault;
        alarms.timestamp = ts;
        alarms.put();

        // 8. Periodic console report ─────────────────────────────────────────
        if (iteration % int(SCAN_HZ * 2.0) == 0) {
            print("[" + ts.toString() + "] level=" + level_pct + "%  temp=" + temp_c +
                "C  pump=" + (pump_running ? "RUN@" + pump_speed + "%" : "OFF") +
                    "  inlet_v=" + valves.inlet.position_pct + "%  outlet_v=" + valves.outlet.position_pct +
                        "%  alarms=" + active_count + (e_stop ? "  [E-STOP]" : "") + "\n");
        }

        iteration++;
        sleep(int(1000.0 / SCAN_HZ));
    }
}
