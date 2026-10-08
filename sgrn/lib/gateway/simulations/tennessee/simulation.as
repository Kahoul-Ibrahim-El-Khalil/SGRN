// simulation.as — Tennessee Eastman surrogate running on s7shell (Soft-PLC).
//
// Simplified physics-informed surrogate of Downs & Vogel (1993), tuned to run
// at 1 Hz on the SGRN shadow memory. It mirrors the Python surrogate in
// ./demo_model.py (same base operating point, same fault IDs) so historian
// records and offline ML datasets stay compatible.
//
// Fault schedule (PlantWide.fault_code):
//   0 = normal (IDV 0)            4 = reactor cooling water step (IDV 4)
//   1 = A/C feed ratio step (IDV 1)  5 = A feed loss step (IDV 6)
//   2 = B composition step (IDV 2)   6 = C header pressure loss (IDV 7)
//   3 = D feed temp step (IDV 3)     7 = feed composition random (IDV 8)
//                                   8 = cooling water random (IDV 11)
// Set PlantWide.fault_code via the Gateway REST API to inject a fault live:
//   POST /data/PlantWide {"fault_code": 4}

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";
PlcRuntime@ plc = PlcRuntime(SCHEMA_PATH);
GatewaySync@ sync = null;

bool setupEnv(const string &in ws_url = GATEWAY_WS_URL) {
    print("================================================================\n");
    print("  Connecting simulation runtime to gateway at " + ws_url + "\n");
    @sync = GatewaySync(plc);
    sync.useBinary(true);
    sync.publishOnDirty(true);
    bool ok = sync.connect(ws_url);
    if (ok) print("  RuntimeSync started — DB updates will flow over WebSocket.\n");
    else print("  RuntimeSync startup failed: " + sync.lastError() + "\n");
    print("================================================================\n");
    return ok;
}

void printAll() {
    if (db1 !is null) { print("--- Reactor (DB1) ---\n"); db1.print(); }
    if (db2 !is null) { print("--- Separator (DB2) ---\n"); db2.print(); }
    if (db3 !is null) { print("--- Stripper (DB3) ---\n"); db3.print(); }
    if (db4 !is null) { print("--- Compressor (DB4) ---\n"); db4.print(); }
    if (db5 !is null) { print("--- PlantWide (DB5) ---\n"); db5.print(); }
}

// ─── Deterministic PRNG (LCG) for process/measurement noise ──────────────────
uint rand_state = 777;
double noise(double amp) {
    rand_state = (1103515245 * rand_state + 12345) % 2147483648;
    double frac = double(rand_state) / 2147483648.0;
    return (frac * 2.0 - 1.0) * amp;
}

// ─── Base operating point (Downs & Vogel nominal steady state, rounded) ─────
double r_pressure = 2700.0;   // kPa   XMEAS 7
double r_level    = 75.0;     // %     XMEAS 8
double r_temp     = 120.0;    // degC  XMEAS 9
double a_feed     = 0.25;     // kscm  XMEAS 1
double d_feed     = 63.0;     // kg/h  XMEAS 2
double e_feed     = 54.0;     // kg/h  XMEAS 3
double ac_feed    = 0.25;     // kscm  XMEAS 4
double recycle    = 32.0;     // kscm  XMEAS 5
double r_feedrate = 42.0;     // kscm  XMEAS 6
double purge      = 0.34;     // kscm  XMEAS 10
double sep_temp   = 83.0;     // degC  XMEAS 11
double sep_level  = 50.0;     // %     XMEAS 12
double sep_press  = 2600.0;   // kPa   XMEAS 13
double sep_under  = 26.0;     // m3/h  XMEAS 14
double str_level  = 50.0;     // %     XMEAS 15
double str_press  = 2600.0;   // kPa   XMEAS 16
double str_under  = 25.0;     // m3/h  XMEAS 17
double str_temp   = 66.0;     // degC  XMEAS 18
double steam_flow = 230.0;    // kg/h  XMEAS 19
double comp_work  = 280.0;    // kW    XMEAS 20
double r_cw_out   = 92.0;     // degC  XMEAS 21
double s_cw_out   = 88.0;     // degC  XMEAS 22

double comp_a = 32.0; double comp_b = 14.0; double comp_c = 22.0; double comp_d = 10.0;
double comp_e = 18.0; double comp_f = 2.0;  double comp_g = 1.5; double comp_h = 0.5;

double purge_valve_pos = 30.0;
double cooling_valve_pos = 50.0;
double steam_valve_pos = 50.0;
double recycle_valve_pos = 50.0;

int fault_code = 0;
int tick = 0;

void applyFaultStep() {
    // Step / random disturbances keyed on fault_code. Magnitudes match the
    // Python surrogate so offline thresholds transfer to live runs.
    if (fault_code == 1) { ac_feed += 0.10; a_feed -= 0.05; }                 // A/C ratio
    else if (fault_code == 2) { comp_b += 0.35; comp_a -= 0.20; }             // B composition
    else if (fault_code == 3) { d_feed += 4.0; r_temp += 0.25; }               // D feed temp
    else if (fault_code == 4) { r_cw_out += 0.60; r_temp += 0.18; }           // reactor cooling
    else if (fault_code == 5) { a_feed -= 0.12; r_pressure -= 3.0; }          // A feed loss
    else if (fault_code == 6) { ac_feed -= 0.08; recycle -= 0.30; r_pressure -= 2.0; } // C header
    else if (fault_code == 7) { comp_a += noise(0.8); comp_c += noise(0.6); r_temp += noise(0.3); }
    else if (fault_code == 8) { r_cw_out += noise(1.2); r_temp += noise(0.5); }
}

void regulate() {
    // Base-layer PI-ish regulation back toward nominal (keeps the demo stable
    // without a full decentralized control structure).
    r_pressure += (2700.0 - r_pressure) * 0.05 + noise(4.0);
    r_level    += (75.0 - r_level)       * 0.05 + noise(0.15);
    r_temp     += (120.0 - r_temp)       * 0.05 + noise(0.10);
    sep_level  += (50.0 - sep_level)     * 0.05 + noise(0.15);
    sep_press  += (2600.0 - sep_press)   * 0.05 + noise(3.0);
    str_level  += (50.0 - str_level)     * 0.05 + noise(0.15);
    recycle    += (32.0 - recycle)       * 0.03 + noise(0.10);
    purge      += (0.34 - purge)         * 0.03 + noise(0.004);
    comp_work  += (280.0 - comp_work)    * 0.03 + noise(1.0);
    sep_temp   += (83.0 - sep_temp)      * 0.05 + noise(0.10);
    str_temp   += (66.0 - str_temp)      * 0.05 + noise(0.10);
    steam_flow += (230.0 - steam_flow)   * 0.03 + noise(1.5);
}

void main() {
    print("================================================================\n");
    print("  SGRN Tennessee Eastman Process Simulator (surrogate, 1 Hz)   \n");
    print("================================================================\n");
    if (!setupEnv()) {
        print("ERROR: Failed to set up environment. Aborting simulation.\n");
        return;
    }
    int iteration = 0;
    while (true) {
        DTL@ ts = dtl();
        // Allow live fault injection from the Gateway twin / operator UI.
        if (db5 !is null) fault_code = db5.fault_code;
        applyFaultStep();
        regulate();
        tick++;

        writeReactor(db1, ts);
        writeSeparator(db2, ts);
        writeStripper(db3, ts);
        writeCompressor(db4, ts);
        writePlantWide(db5, ts);

        if (iteration % 5 == 0) { printReport(ts, iteration); }
        iteration++;
        sleep(1000);
    }
}

void writeReactor(Reactor@ db, DTL@ ts) {
    db.a_feed_flow = float(a_feed + noise(0.004));
    db.d_feed_flow = float(d_feed + noise(0.4));
    db.e_feed_flow = float(e_feed + noise(0.4));
    db.ac_feed_flow = float(ac_feed + noise(0.004));
    db.reactor_feed_rate = float(r_feedrate + noise(0.10));
    db.pressure = float(r_pressure);
    db.level = float(r_level);
    db.temp = float(r_temp);
    db.cooling_out_temp = float(r_cw_out + noise(0.15));
    db.feed_comp.comp_a = float(comp_a); db.feed_comp.comp_b = float(comp_b);
    db.feed_comp.comp_c = float(comp_c); db.feed_comp.comp_d = float(comp_d);
    db.feed_comp.comp_e = float(comp_e); db.feed_comp.comp_f = float(comp_f);
    db.feed_comp.comp_g = float(comp_g); db.feed_comp.comp_h = float(comp_h);
    db.d_feed_valve.position_pct = 63.0f; db.d_feed_valve.command_pct = 63.0f;
    db.d_feed_valve.fault = false; db.d_feed_valve.limit_open = true; db.d_feed_valve.limit_closed = false;
    db.e_feed_valve.position_pct = 54.0f; db.e_feed_valve.command_pct = 54.0f;
    db.e_feed_valve.fault = false; db.e_feed_valve.limit_open = true; db.e_feed_valve.limit_closed = false;
    db.a_feed_valve.position_pct = 55.0f; db.a_feed_valve.command_pct = 55.0f;
    db.a_feed_valve.fault = false; db.a_feed_valve.limit_open = true; db.a_feed_valve.limit_closed = false;
    db.ac_feed_valve.position_pct = 55.0f; db.ac_feed_valve.command_pct = 55.0f;
    db.ac_feed_valve.fault = false; db.ac_feed_valve.limit_open = true; db.ac_feed_valve.limit_closed = false;
    db.cooling_valve.position_pct = float(cooling_valve_pos); db.cooling_valve.command_pct = float(cooling_valve_pos);
    db.cooling_valve.fault = false;
    db.pressure_pid.setpoint = 2700.0; db.pressure_pid.process_value = r_pressure;
    db.pressure_pid.output_pct = 50.0f; db.pressure_pid.enabled = true; db.pressure_pid.auto_mode = true;
    db.level_pid.setpoint = 75.0; db.level_pid.process_value = r_level;
    db.level_pid.output_pct = 50.0f; db.level_pid.enabled = true; db.level_pid.auto_mode = true;
    db.temp_pid.setpoint = 120.0; db.temp_pid.process_value = r_temp;
    db.temp_pid.output_pct = 50.0f; db.temp_pid.enabled = true; db.temp_pid.auto_mode = true;
    db.timestamp = ts;;
    db.put();
}

void writeSeparator(Separator@ db, DTL@ ts) {
    db.purge_rate = float(purge);
    db.product_temp = float(sep_temp);
    db.sep_level = float(sep_level);
    db.sep_pressure = float(sep_press);
    db.sep_underflow = float(sep_under + noise(0.10));
    db.cooling_out_temp = float(s_cw_out + noise(0.15));
    db.purge_comp.comp_a = float(comp_a * 0.9); db.purge_comp.comp_b = float(comp_b * 0.9);
    db.purge_comp.comp_c = float(comp_c * 0.9); db.purge_comp.comp_d = float(comp_d);
    db.purge_comp.comp_e = float(comp_e); db.purge_comp.comp_f = float(comp_f);
    db.purge_comp.comp_g = float(comp_g); db.purge_comp.comp_h = float(comp_h);
    db.purge_valve.position_pct = float(purge_valve_pos); db.purge_valve.command_pct = float(purge_valve_pos);
    db.purge_valve.fault = false;
    db.sep_pot_valve.position_pct = 50.0f; db.sep_pot_valve.command_pct = 50.0f; db.sep_pot_valve.fault = false;
    db.condenser_valve.position_pct = 50.0f; db.condenser_valve.command_pct = 50.0f; db.condenser_valve.fault = false;
    db.level_pid.setpoint = 50.0; db.level_pid.process_value = sep_level;
    db.level_pid.output_pct = 50.0f; db.level_pid.enabled = true; db.level_pid.auto_mode = true;
    db.pressure_pid.setpoint = 2600.0; db.pressure_pid.process_value = sep_press;
    db.pressure_pid.output_pct = 50.0f; db.pressure_pid.enabled = true; db.pressure_pid.auto_mode = true;
    db.timestamp = ts;;
    db.put();
}

void writeStripper(Stripper@ db, DTL@ ts) {
    db.level = float(str_level);
    db.pressure = float(str_press + noise(2.0));
    db.underflow = float(str_under + noise(0.10));
    db.temp = float(str_temp);
    db.steam_flow = float(steam_flow);
    db.product_comp.comp_a = 0.1f; db.product_comp.comp_b = 0.1f; db.product_comp.comp_c = 0.1f;
    db.product_comp.comp_d = float(comp_d * 0.8); db.product_comp.comp_e = float(comp_e * 0.8);
    db.product_comp.comp_f = float(comp_f); db.product_comp.comp_g = float(comp_g * 2.0);
    db.product_comp.comp_h = float(comp_h * 2.0);
    db.product_valve.position_pct = 50.0f; db.product_valve.command_pct = 50.0f; db.product_valve.fault = false;
    db.steam_valve.position_pct = float(steam_valve_pos); db.steam_valve.command_pct = float(steam_valve_pos);
    db.steam_valve.fault = false;
    db.level_pid.setpoint = 50.0; db.level_pid.process_value = str_level;
    db.level_pid.output_pct = 50.0f; db.level_pid.enabled = true; db.level_pid.auto_mode = true;
    db.timestamp = ts;;
    db.put();
}

void writeCompressor(Compressor@ db, DTL@ ts) {
    db.recycle_flow = float(recycle);
    db.work = float(comp_work);
    db.recycle_valve.position_pct = float(recycle_valve_pos); db.recycle_valve.command_pct = float(recycle_valve_pos);
    db.recycle_valve.fault = false;
    db.agitator_speed = 100.0f;
    db.timestamp = ts;;
    db.put();
}

void writePlantWide(PlantWide@ db, DTL@ ts) {
    db.fault_code = fault_code;
    db.fault_active = fault_code != 0;
    db.op_mode = (r_pressure > 3000.0 || r_temp > 150.0) ? 2 : (fault_code != 0 ? 1 : 0);
    db.production_rate = float(str_under * 40.0);
    db.operating_cost = float(comp_work * 0.05 + steam_flow * 0.01 + purge * 50.0);
    db.trip_high_pressure_bar = 3000.0f;
    db.trip_high_temp = 150.0f;
    db.trip_high_level = 100.0f;
    bool trip = (r_pressure > 3000.0 || r_temp > 150.0 || r_level > 100.0);
    db.tripped = trip;
    db.trip_reason_code = trip ? 7001 : 0;
    db.recommended_action = 0;
    db.action_confidence = 99.0f;
    db.action_blocked = false;
    db.action_explanation = 0;
    db.alarms[0].active = fault_code != 0;
    db.alarms[0].acknowledged = false;
    db.alarms[0].code = uint16(7000 + fault_code);
    db.alarms[0].priority = uint8(fault_code == 0 ? 0 : 2);
    db.alarms[0].description_id = uint(fault_code);
    db.alarms[0].timestamp_ms = 0;
    for (int i = 1; i < 8; i++) {
        db.alarms[i].active = false; db.alarms[i].acknowledged = false;
        db.alarms[i].code = 0; db.alarms[i].priority = 0;
        db.alarms[i].description_id = 0; db.alarms[i].timestamp_ms = 0;
    }
    db.active_alarm_count = uint(fault_code == 0 ? 0 : 1);
    db.any_critical = trip;
    db.timestamp = ts;;
    db.put();
}

void printReport(DTL@ ts, int iter) {
    print("\n================================================================\n");
    print("  Time: " + ts.toString() + " | Iteration: " + iter + " | Fault: " + fault_code + "\n");
    print("----------------------------------------------------------------\n");
    print("  Reactor P/T/L:        " + r_pressure + " kPa / " + r_temp + " C / " + r_level + " %\n");
    print("  Separator P/L:        " + sep_press + " kPa / " + sep_level + " %\n");
    print("  Stripper L/T:         " + str_level + " % / " + str_temp + " C\n");
    print("  Recycle / Purge:      " + recycle + " / " + purge + " kscm\n");
    if (fault_code != 0) { print("  Status:               FAULT " + fault_code + " INJECTED\n"); }
    else { print("  Status:               NORMAL OPERATION\n"); }
    print("================================================================\n\n");
}
