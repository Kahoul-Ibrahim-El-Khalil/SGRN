// simulation.as — LLM-in-the-loop Tank/Pump Skid Simulator
//
// Simple physics-informed surrogate running at 1 Hz on SGRN shadow memory.
// Includes periodic LLM analysis requests for plant state assessment.

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";
const int LLM_ANALYSIS_INTERVAL_S = 10;  // Request LLM analysis every 10 seconds

PlcRuntime@ plc = PlcRuntime(SCHEMA_PATH);
GatewaySync@ sync = null;

// ─── Deterministic PRNG (LCG) for process/measurement noise ──────────────────
uint rand_state = 777;
double noise(double amp) {
    rand_state = (1103515245 * rand_state + 12345) % 2147483648;
    double frac = double(rand_state) / 2147483648.0;
    return (frac * 2.0 - 1.0) * amp;
}

double clamp(double val, double min, double max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

// ─── Base operating point ─────────────────────────────────────────────────────
double tank_level = 50.0;      // %
double tank_volume = 500.0;    // L
double tank_temp = 25.0;       // °C
double inlet_flow = 0.0;       // L/min
double outlet_flow = 0.0;      // L/min
double pump_speed = 0.0;       // %
bool pump_running = false;
double pump_runtime = 0.0;     // s
double inlet_valve = 0.0;      // %
double outlet_valve = 0.0;     // %
bool heater_on = false;
double heater_setpoint = 60.0; // °C
double heater_kp = 2.0;
double heater_ki = 0.1;
double heater_integral = 0.0;
bool heater_enabled = false;

bool high_level_alarm = false;
bool low_level_alarm = false;
bool overfill_trip = false;
bool pump_fault = false;
bool e_stop = false;

int skid_mode = 0;  // 0=OFF, 1=FILLING, 2=HEATING, 3=DISCHARGING, 4=ALARM
bool fault_active = false;

bool gateway_connected = false;

int iteration = 0;
int llm_request_counter = 0;
int last_llm_request_iter = -100;

bool setupEnv(const string &in ws_url = GATEWAY_WS_URL) {
    print("================================================================\n");
    print("  Connecting LLM-in-loop simulation to gateway at " + ws_url + "\n");
    @sync = GatewaySync(plc);
    sync.useBinary(true);
    sync.publishOnDirty(true);
    bool ok = sync.connect(ws_url);
    if (ok) {
        print("  RuntimeSync started — DB updates will flow over WebSocket.\n");
        gateway_connected = true;
    } else {
        print("  RuntimeSync startup failed: " + sync.lastError() + "\n");
    }
    print("================================================================\n");
    return ok;
}

void printAll() {
    if (db1 !is null) { print("--- Setpoints (DB1) ---\n"); db1.print(); }
    if (db2 !is null) { print("--- Tank (DB2) ---\n"); db2.print(); }
    if (db3 !is null) { print("--- Pump (DB3) ---\n"); db3.print(); }
    if (db4 !is null) { print("--- Valves (DB4) ---\n"); db4.print(); }
    if (db5 !is null) { print("--- Heater (DB5) ---\n"); db5.print(); }
    if (db6 !is null) { print("--- Alarms (DB6) ---\n"); db6.print(); }
    if (db7 !is null) { print("--- LLMAnalysis (DB7) ---\n"); db7.print(); }
}

void applySetpoints() {
    if (db1 !is null) {
        pump_running = db1.pump_run;
        pump_speed = clamp(db1.pump_speed_sp, 0.0, 100.0);
        inlet_valve = clamp(db1.inlet_valve_cmd, 0.0, 100.0);
        outlet_valve = clamp(db1.outlet_valve_cmd, 0.0, 100.0);
        heater_enabled = db1.heater_enable;
        heater_setpoint = db1.heater_setpoint;
        e_stop = db1.e_stop;
        
        if (db1.ack_alarms) {
            high_level_alarm = false;
            low_level_alarm = false;
            overfill_trip = false;
        }
    }
}

void updatePhysics() {
    if (e_stop) {
        pump_running = false;
        pump_speed = 0.0;
        inlet_valve = 0.0;
        outlet_valve = 0.0;
        heater_enabled = false;
        skid_mode = 4; // ALARM
        fault_active = true;
    }
    
    if (pump_running && !pump_fault) {
        inlet_flow = (pump_speed / 100.0) * (inlet_valve / 100.0) * 50.0;
        pump_runtime += 1.0;
        skid_mode = (tank_temp < heater_setpoint - 2.0) ? 2 : 1;
    } else {
        inlet_flow = 0.0;
        if (!pump_running && tank_level > 10.0) skid_mode = 3;
        else if (!pump_running) skid_mode = 0;
    }
    
    outlet_flow = (outlet_valve / 100.0) * 30.0;
    
    double net_flow = inlet_flow - outlet_flow;
    tank_volume += net_flow / 60.0;
    tank_volume = clamp(tank_volume, 0.0, 1000.0);
    tank_level = (tank_volume / 1000.0) * 100.0;
    
    if (tank_level >= 95.0) { high_level_alarm = true; overfill_trip = true; }
    if (tank_level <= 5.0) { low_level_alarm = true; }
    
    if (heater_enabled && heater_on && tank_volume > 50.0) {
        double temp_error = heater_setpoint - tank_temp;
        heater_integral += temp_error * (1.0/60.0);
        heater_integral = clamp(heater_integral, -100.0, 100.0);
        double heater_power = heater_kp * temp_error + heater_ki * heater_integral;
        heater_power = clamp(heater_power, 0.0, 100.0);
        tank_temp += (heater_power / 100.0) * (2.0 / 60.0) + noise(0.02);
    } else {
        tank_temp += (20.0 - tank_temp) * 0.001 + noise(0.01);
        heater_integral *= 0.99;
    }
    tank_temp = clamp(tank_temp, 0.0, 120.0);
    
    if (db4 !is null) {
        double inlet_target = inlet_valve;
        double outlet_target = outlet_valve;
        db4.inlet.position += (inlet_target - db4.inlet.position) * 0.1;
        db4.outlet.position += (outlet_target - db4.outlet.position) * 0.1;
    }
    
    if (high_level_alarm || low_level_alarm || pump_fault || e_stop) {
        skid_mode = 4; fault_active = true;
    } else if (heater_enabled && tank_temp < heater_setpoint - 1.0) {
        skid_mode = 2;
    } else if (pump_running && inlet_flow > 1.0) {
        skid_mode = 1;
    } else if (outlet_flow > 1.0) {
        skid_mode = 3;
    } else {
        skid_mode = 0;
    }
}

void requestLLMAnalysis() {
    if (db7 !is null && !db7.request_pending) {
        llm_request_counter++;
        db7.request_pending = true;
        db7.request_id = llm_request_counter;
        db7.last_error = "";
        print("[SIM] LLM analysis requested (ID: " + llm_request_counter + ")\n");
    }
}

void applyLLMAction() {
    if (db7 !is null && !db7.request_pending && db7.response.suggested_action != 0) {
        int action = db7.response.suggested_action;
        double confidence = db7.response.action_confidence;
        
        if (confidence > 70.0) {
            print("[SIM] Applying LLM action: " + action + " (confidence: " + confidence + "%)\n");
            
            switch (action) {
                case 1: // INCREASE_FILL
                    if (db1 !is null) {
                        db1.pump_run = true;
                        db1.pump_speed_sp = clamp(db1.pump_speed_sp + 10.0, 0.0, 100.0);
                        db1.inlet_valve_cmd = clamp(db1.inlet_valve_cmd + 10.0, 0.0, 100.0);
                    }
                    break;
                case 2: // DECREASE_FILL
                    if (db1 !is null) {
                        db1.pump_speed_sp = clamp(db1.pump_speed_sp - 10.0, 0.0, 100.0);
                        db1.inlet_valve_cmd = clamp(db1.inlet_valve_cmd - 10.0, 0.0, 100.0);
                        if (db1.pump_speed_sp <= 10.0) db1.pump_run = false;
                    }
                    break;
                case 3: // START_HEAT
                    if (db1 !is null) {
                        db1.heater_enable = true;
                        db1.heater_setpoint = 60.0;
                    }
                    break;
                case 4: // STOP_HEAT
                    if (db1 !is null) { db1.heater_enable = false; }
                    break;
                case 5: // OPEN_OUTLET
                    if (db1 !is null) {
                        db1.outlet_valve_cmd = clamp(db1.outlet_valve_cmd + 20.0, 0.0, 100.0);
                    }
                    break;
                case 6: // CLOSE_OUTLET
                    if (db1 !is null) {
                        db1.outlet_valve_cmd = clamp(db1.outlet_valve_cmd - 20.0, 0.0, 100.0);
                    }
                    break;
                case 7: // EMERGENCY_STOP
                    if (db1 !is null) { db1.e_stop = true; }
                    break;
                default: break;
            }
        }
        db7.response.suggested_action = 0;
    }
}

void writeSetpoints(Setpoints@ db, DTL@ ts) {
    db.pump_run = pump_running;
    db.pump_speed_sp = float(pump_speed);
    db.inlet_valve_cmd = float(inlet_valve);
    db.outlet_valve_cmd = float(outlet_valve);
    db.heater_enable = heater_enabled;
    db.heater_setpoint = float(heater_setpoint);
    db.e_stop = e_stop;
    db.ack_alarms = false;
    db.timestamp = ts;
    db.put();
}

void writeTank(Tank@ db, DTL@ ts) {
    db.level = float(tank_level);
    db.volume = float(tank_volume);
    db.temperature = float(tank_temp);
    db.inlet_flow = float(inlet_flow);
    db.outlet_flow = float(outlet_flow);
    db.high_level_alarm = high_level_alarm;
    db.low_level_alarm = low_level_alarm;
    db.overfill_trip = overfill_trip;
    db.timestamp = ts;
    db.put();
}

void writePump(Pump@ db, DTL@ ts) {
    db.running = pump_running;
    db.speed = float(pump_speed);
    db.fault = pump_fault;
    db.runtime = pump_runtime;
    db.timestamp = ts;
    db.put();
}

void writeValves(Valves@ db, DTL@ ts) {
    db.inlet.position = float(db4.inlet.position);
    db.inlet.command = float(inlet_valve);
    db.inlet.travel_time = 5.0;
    db.inlet.fault = false;
    db.inlet.limit_open = (db.inlet.position > 99.0);
    db.inlet.limit_closed = (db.inlet.position < 1.0);
    
    db.outlet.position = float(db4.outlet.position);
    db.outlet.command = float(outlet_valve);
    db.outlet.travel_time = 5.0;
    db.outlet.fault = false;
    db.outlet.limit_open = (db.outlet.position > 99.0);
    db.outlet.limit_closed = (db.outlet.position < 1.0);
    
    db.timestamp = ts;
    db.put();
}

void writeHeater(Heater@ db, DTL@ ts) {
    db.loop.setpoint = heater_setpoint;
    db.loop.process_value = tank_temp;
    db.loop.output = heater_enabled ? float(clamp((heater_setpoint - tank_temp) * 5.0, 0.0, 100.0)) : 0.0;
    db.loop.kp = float(heater_kp);
    db.loop.ki = float(heater_ki);
    db.loop.kd = 0.0;
    db.loop.integral_acc = heater_integral;
    db.loop.enabled = heater_enabled;
    db.loop.auto_mode = true;
    db.loop.saturated = (db.loop.output >= 99.0 || db.loop.output <= 1.0);
    db.timestamp = ts;
    db.put();
}

void writeAlarms(Alarms@ db, DTL@ ts) {
    db.any_active = high_level_alarm || low_level_alarm || pump_fault || e_stop;
    db.active_count = uint((high_level_alarm?1:0) + (low_level_alarm?1:0) + (pump_fault?1:0) + (e_stop?1:0));
    db.e_stop_active = e_stop;
    db.high_level = high_level_alarm;
    db.low_level = low_level_alarm;
    db.pump_fault = pump_fault;
    db.timestamp = ts;
    db.put();
}

void initLLMAnalysis(LLMAnalysis@ db, DTL@ ts) {
    db.response.analysis_text = "";
    db.response.suggested_action = 0;
    db.response.action_confidence = 0.0;
    db.response.reasoning = "";
    db.response.timestamp_ms = 0;
    db.response.model_name = "";
    db.prev_response.analysis_text = "";
    db.prev_response.suggested_action = 0;
    db.prev_response.action_confidence = 0.0;
    db.prev_response.reasoning = "";
    db.prev_response.timestamp_ms = 0;
    db.prev_response.model_name = "";
    db.request_pending = false;
    db.request_id = 0;
    db.last_error = "";
    db.put();
}

void printReport(DTL@ ts, int iter) {
    print("\n================================================================\n");
    print("  Time: " + ts.toString() + " | Iter: " + iter + " | Mode: " + skid_mode + "\n");
    print("----------------------------------------------------------------\n");
    print("  Tank:       " + tank_level + "% (" + tank_volume + "L) | " + tank_temp + "°C\n");
    print("  Flows:      In=" + inlet_flow + " L/min | Out=" + outlet_flow + " L/min\n");
    print("  Pump:       " + (pump_running?"RUN":"STOP") + " @ " + pump_speed + "%\n");
    print("  Valves:     In=" + inlet_valve + "% | Out=" + outlet_valve + "%\n");
    print("  Heater:     " + (heater_enabled?"ON":"OFF") + " SP=" + heater_setpoint + "°C\n");
    print("  Alarms:     HL=" + (high_level_alarm?"Y":"N") + " LL=" + (low_level_alarm?"Y":"N") + " EStop=" + (e_stop?"Y":"N") + "\n");
    if (gateway_connected && db7 !is null && iteration > 20) {
        if (db7.response !is null && db7.response.analysis_text.length() > 0) {
            string analysis = db7.response.analysis_text;
            if (analysis.length() > 80) analysis = analysis.substr(0, 80);
            print("  LLM:        " + analysis + "...\n");
            print("  Action:     " + db7.response.suggested_action + " (conf: " + db7.response.action_confidence + "%)\n");
        } else if (db7.request_pending) {
            print("  LLM:        [analysis requested, waiting...]\n");
        }
    }
    print("================================================================\n\n");
}

void main() {
    print("================================================================\n");
    print("  SGRN LLM-in-the-loop Tank/Pump Skid Simulator (1 Hz)        \n");
    print("================================================================\n");
    if (!setupEnv()) {
        print("ERROR: Failed to set up environment. Aborting simulation.\n");
        return;
    }
    
    while (true) {
        DTL@ ts = dtl();
        
        applySetpoints();
        updatePhysics();
        
        if (iteration - last_llm_request_iter >= LLM_ANALYSIS_INTERVAL_S) {
            requestLLMAnalysis();
            last_llm_request_iter = iteration;
        }
        
        applyLLMAction();
        
        writeSetpoints(db1, ts);
        writeTank(db2, ts);
        writePump(db3, ts);
        writeValves(db4, ts);
        writeHeater(db5, ts);
        writeAlarms(db6, ts);
        
        // Write DB7 with sync counter to keep gateway sync active for LLMAnalysis reads
        if (db7 !is null) {
            db7.sync_counter = db7.sync_counter + 1;
            db7.put();
        }
        
        if (iteration == 0) {
            initLLMAnalysis(db7, ts);
        }
        
        if (iteration % 5 == 0) { printReport(ts, iteration); }
        iteration++;
        sleep(1000);
    }
}