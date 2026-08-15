// ============================================================================
// simulation.as — Comprehensive SGRN S7-Shell Process Simulation (OOP Edition)
// Schema: simple.scl  (DB1-Scalars, DB2-Motors, DB3-Sensors,
//                     DB4-Control, DB5-Alarms, DB6-Diagnostics)
// ============================================================================

// ── Simulation state ──────────────────────────────────────────────────────────

double tank_level    = 50.0;
double tank_temp     = 20.0;
double tank_pressure = 1.013;
bool   system_on     = true;
bool   e_stop        = false;
int    motor_hours_0 = 1240;
int    motor_hours_1 = 830;
int    motor_hours_2 = 0;
int    motor_hours_3 = 2055;
double pid_integral  = 0.0;
double pid_setpoint  = 65.0;

// ── Main ──────────────────────────────────────────────────────────────────────

void main() {
    print("=== SGRN Comprehensive Simulation — simple.scl (OOP Edition) ===\n");
    S7Client@ plc = S7Client("127.0.0.1", 0, 1);
    const string schema = "schemas/simple.scl"; 
    plc.loadSclSchema(schema);

    // Cast raw DataBlock pointers to TIA Portal type definitions
    Scalars@ db1 = cast<Scalars>(plc.db(1));
    Motors@ db2 = cast<Motors>(plc.db(2));
    Sensors@ db3 = cast<Sensors>(plc.db(3));
    Control@ db4 = cast<Control>(plc.db(4));
    Alarms@ db5 = cast<Alarms>(plc.db(5));
    Diagnostics@ db6 = cast<Diagnostics>(plc.db(6));

    // Initialize constant values
    writeDiagnostics(db6);
    writeInitialPID(db4);

    int iteration = 0;
    while (true) {
        DTL@ ts = dtl();

        // Write simulation variables natively
        writeScalars(db1, ts, iteration);
        writeSensors(db3, ts, iteration);
        writeMotors(db2, ts, iteration);
        writePID(db4, ts, iteration);
        writeAlarms(db5, ts, iteration);

        if (iteration % 5 == 0) {
            printReport(db1, db2, db3, db4, db5, db6, ts, iteration);
        }
        iteration++;
        
        if (tank_level > 95.0) { system_on = false; e_stop = true; }
        if (!system_on && tank_level < 30.0) { system_on = true; e_stop = false; }
        
        sleep(1000);
    }
}

void writeScalars(Scalars@ db, DTL@ ts, int iter) {
    db.flag_bool = system_on;
    db.val_sint = (iter % 200) - 100;
    db.val_int = (iter % 60000) - 30000;
    db.val_real = float(3.14159 * (iter + 1));
    db.val_lreal = 2.71828 * iter;
    db.val_string = "iter=" + iter + " lvl=" + tank_level;
    
    // Assign nested UDT fields cleanly
    db.deep_nest.tag = "NEST-" + iter;
    db.deep_nest.lvl2.flag = (iter % 2 == 0);
    db.deep_nest.lvl2.lvl3.value_udint = 1000 + iter;
    db.deep_nest.lvl2.lvl3.value_real = 1.234f * (iter + 1);
    
    db.timestamp = ts;
    db.put();
}

void writeSensors(Sensors@ db, DTL@ ts, int iter) {
    db.temperatures[0] = tank_temp;
    db.temperatures[1] = tank_temp + 1;
    db.temperatures[2] = tank_temp + 2;
    db.temperatures[3] = tank_temp + 3;
    db.temperatures[4] = 20.0f;
    db.temperatures[5] = 21.0f;
    db.temperatures[6] = 22.0f;
    db.temperatures[7] = 23.0f;

    db.pressures[0] = tank_pressure;
    db.pressures[1] = tank_pressure + 0.1;
    db.pressures[2] = 1.0f;
    db.pressures[3] = 1.1f;

    db.encoders[0] = double(iter * 10);
    db.encoders[1] = double(iter * 20);
    db.encoders[2] = 0;
    db.encoders[3] = 0;
    db.encoders[4] = 0;
    db.encoders[5] = 0;
    db.encoders[6] = 0;
    db.encoders[7] = 0;

    db.flow_totals[0] = iter * 5;
    db.flow_totals[1] = iter * 2;
    db.flow_totals[2] = 0;
    db.flow_totals[3] = 0;

    db.analog_in[0] = iter % 100;
    db.analog_in[1] = 200;
    db.analog_in[2] = 300;
    db.analog_in[3] = 400;
    db.analog_in[4] = 500;
    db.analog_in[5] = 600;

    db.analog_out[0] = 500;
    db.analog_out[1] = 600;
    db.analog_out[2] = 700;
    db.analog_out[3] = 800;
    db.analog_out[4] = 900;
    db.analog_out[5] = 1000;

    for (int i = 0; i < 16; i++) {
        db.digital_in[i] = (i % 2 == 0);
    }

    db.timestamp = ts;
    db.put();
}

void writeMotors(Motors@ db, DTL@ ts, int iter) {
    bool drv_run = system_on && !e_stop;
    if (drv_run) motor_hours_0++;
    db.drive.running = drv_run;
    db.drive.speed_rpm = drv_run ? 1400 + (iter % 50) : 0;
    db.drive.stats.active = drv_run;
    db.drive.stats.health = drv_run ? 95 : 0;
    
    // Unit 0
    db.units[0].running = drv_run;
    db.units[0].fault = false;
    db.units[0].speed_rpm = 1450;
    db.units[0].torque_nm = 38.0;
    db.units[0].power_kw = 15.2;
    db.units[0].run_hours = motor_hours_0;
    db.units[0].fault_code = 0;
    db.units[0].status_word = 257;
    db.units[0].control_word = 15;
    db.units[0].phase_current = 12;
    db.units[0].stats.active = drv_run;
    db.units[0].stats.health = 95;
    db.units[0].stats.last_error = 0;

    // Unit 1
    db.units[1].running = false;
    db.units[1].fault = true;
    db.units[1].speed_rpm = 0;
    db.units[1].torque_nm = 0.0;
    db.units[1].power_kw = 0.0;
    db.units[1].run_hours = motor_hours_1;
    db.units[1].fault_code = 3;
    db.units[1].status_word = 2;
    db.units[1].control_word = 0;
    db.units[1].phase_current = 0;
    db.units[1].stats.active = false;
    db.units[1].stats.health = 88;
    db.units[1].stats.last_error = 3;

    // Unit 2
    db.units[2].running = false;
    db.units[2].fault = false;
    db.units[2].speed_rpm = 0;
    db.units[2].torque_nm = 0.0;
    db.units[2].power_kw = 0.0;
    db.units[2].run_hours = 0;
    db.units[2].fault_code = 0;
    db.units[2].status_word = 0;
    db.units[2].control_word = 0;
    db.units[2].phase_current = 0;
    db.units[2].stats.active = false;
    db.units[2].stats.health = 0;
    db.units[2].stats.last_error = 0;

    // Unit 3
    db.units[3].running = true;
    db.units[3].fault = false;
    db.units[3].speed_rpm = 960;
    db.units[3].torque_nm = 95.0;
    db.units[3].power_kw = 45.3;
    db.units[3].run_hours = motor_hours_3;
    db.units[3].fault_code = 0;
    db.units[3].status_word = 257;
    db.units[3].control_word = 15;
    db.units[3].phase_current = 30;
    db.units[3].stats.active = true;
    db.units[3].stats.health = 99;
    db.units[3].stats.last_error = 0;
    
    db.bus_voltage = float(670.0 + (iter % 10));
    db.timestamp = ts;
    db.put();
}

void writePID(Control@ db, DTL@ ts, int iter) {
    db.temp_pid.setpoint = pid_setpoint;
    db.temp_pid.pv = tank_temp;
    
    // Loop 0
    db.loops[0].setpoint = 65.0;
    db.loops[0].pv = tank_temp;
    db.loops[0].output = 100.0;
    db.loops[0].kp = 2.5f;
    db.loops[0].ki = 0.1f;
    db.loops[0].kd = 0.5f;
    db.loops[0].enabled = true;
    db.loops[0].auto_mode = true;
    db.loops[0].mode = 1;
    db.loops[0].integral = 4.5;

    // Loop 1
    db.loops[1].setpoint = 60.0;
    db.loops[1].pv = 50.0;
    db.loops[1].output = 55.0;
    db.loops[1].kp = 3.0f;
    db.loops[1].ki = 0.05f;
    db.loops[1].kd = 0.8f;
    db.loops[1].enabled = true;
    db.loops[1].auto_mode = true;
    db.loops[1].mode = 1;
    db.loops[1].integral = 0.0;

    // Loop 2
    db.loops[2].setpoint = 2.5;
    db.loops[2].pv = tank_pressure;
    db.loops[2].output = 2.1;
    db.loops[2].kp = 1.5f;
    db.loops[2].ki = 0.02f;
    db.loops[2].kd = 0.3f;
    db.loops[2].enabled = true;
    db.loops[2].auto_mode = true;
    db.loops[2].mode = 1;
    db.loops[2].integral = 0.0;

    // Loop 3
    db.loops[3].setpoint = 8.0;
    db.loops[3].pv = 7.8;
    db.loops[3].output = 77.0;
    db.loops[3].kp = 4.0f;
    db.loops[3].ki = 0.08f;
    db.loops[3].kd = 1.2f;
    db.loops[3].enabled = true;
    db.loops[3].auto_mode = true;
    db.loops[3].mode = 1;
    db.loops[3].integral = 0.0;
    
    db.setpoints[0] = 65.0;
    db.setpoints[1] = 60.0;
    db.setpoints[2] = 2.5;
    db.setpoints[3] = 8.0;

    db.system_on = system_on;
    db.timestamp = ts;
    db.put();
}

void writeAlarms(Alarms@ db, DTL@ ts, int iter) {
    db.active_count = (iter % 3 == 0) ? 1 : 0;
    db.total_fired = 1;
    
    db.entries[0].active = false;
    db.entries[0].acknowledged = false;
    db.entries[0].code = 257;
    db.entries[0].priority = 2;
    db.entries[0].msg_id = 101;
    db.entries[0].trigger_ms = iter * 1000;

    db.entries[1].active = false;
    db.entries[1].acknowledged = false;
    db.entries[1].code = 258;
    db.entries[1].priority = 3;
    db.entries[1].msg_id = 102;
    db.entries[1].trigger_ms = iter * 1000;

    db.entries[2].active = false;
    db.entries[2].acknowledged = false;
    db.entries[2].code = 513;
    db.entries[2].priority = 1;
    db.entries[2].msg_id = 201;
    db.entries[2].trigger_ms = iter * 1000;

    db.entries[3].active = (iter % 3 == 0);
    db.entries[3].acknowledged = false;
    db.entries[3].code = 769;
    db.entries[3].priority = 2;
    db.entries[3].msg_id = 301;
    db.entries[3].trigger_ms = iter * 1000;

    db.entries[4].active = false;
    db.entries[4].acknowledged = false;
    db.entries[4].code = 4080;
    db.entries[4].priority = 3;
    db.entries[4].msg_id = 901;
    db.entries[4].trigger_ms = iter * 1000;

    for (int i = 5; i < 8; i++) {
        db.entries[i].active = false;
        db.entries[i].acknowledged = false;
        db.entries[i].code = 0;
        db.entries[i].priority = 0;
        db.entries[i].msg_id = 0;
        db.entries[i].trigger_ms = 0;
    }
    
    db.timestamp = ts;
    db.put();
}

void writeDiagnostics(Diagnostics@ db) {
    db.s_sint = -128;
    db.s_int = -32768;
    db.u_uint = 65535;
    db.f_real_pi = 3.14159f;
    db.f_lreal_pi = 3.1415926535;
    db.f_lreal_neg = -1.79769e+100;
    db.device_name = "SGRN-Gateway-01";
    db.location_tag = "UNIT-A-RACK-3";

    db.sint_arr[0] = -128;
    db.sint_arr[1] = -64;
    db.sint_arr[2] = 0;
    db.sint_arr[3] = 127;

    db.lreal_arr[0] = -3.14159;
    db.lreal_arr[1] = 2.71828;
    db.lreal_arr[2] = 1.41421;
    db.lreal_arr[3] = 1.61803;

    db.timestamp = dtl();
    db.put();
}

void writeInitialPID(Control@ db) {
    db.temp_pid.kp = 2.5f;
    db.temp_pid.ki = 0.1f;
    db.temp_pid.kd = 0.5f;
    db.put();
}

void printReport(Scalars@ db1, Motors@ db2, Sensors@ db3,
                 Control@ db4, Alarms@ db5, Diagnostics@ db6,
                 DTL@ ts, int iter) {
    print("\n╔══════════════════════════════════════════════════════╗\n");
    print("║  Iteration " + iter + " — " + ts.toString() + "\n");
    print("╠══════════════════════════════════════════════════════╣\n");
    print("║ DB1-Scalars\n");
    print("║   flag_bool  = " + (db1.flag_bool ? "true" : "false") + "\n");
    print("║   val_real   = " + db1.val_real + "\n");
    print("║   val_string = " + db1.val_string + "\n");
    print("║   deep_nest.tag = " + db1.deep_nest.tag + "\n");
    print("║   deep_nest.lvl2.lvl3.value_real = " + db1.deep_nest.lvl2.lvl3.value_real + "\n");
    print("║ DB2-Motors (scalar drive)\n");
    print("║   drive.running   = " + (db2.drive.running ? "true" : "false") + "\n");
    print("║   drive.stats.health = " + db2.drive.stats.health + " %\n");
    print("║ DB3-Sensors\n");
    print("║   temperatures[0] = " + db3.temperatures[0] + "\n");
    print("║   pressures[0]    = " + db3.pressures[0] + "\n");
    print("║ DB4-Control\n");
    print("║   temp_pid.setpoint = " + db4.temp_pid.setpoint + "\n");
    print("║ DB5-Alarms\n");
    print("║   active_count = " + db5.active_count + "\n");
    print("║   any_critical = " + (db5.any_critical ? "true" : "false") + "\n");
    print("║ DB6-Diagnostics\n");
    print("║   device_name = " + db6.device_name + "\n");
    print("║   sint_arr[0] = " + db6.sint_arr[0] + "\n");
    print("╚══════════════════════════════════════════════════════╝\n\n");
    
    // Demonstrate hex viewing and live updates using direct .hex() method call chaining
    HexTable@ hex = db1.hex();
    hex.print();

    if (system_on && !e_stop) tank_level += 0.1; else tank_level -= 0.1;
    tank_pressure = 1.013 + tank_level * 0.025;
}
