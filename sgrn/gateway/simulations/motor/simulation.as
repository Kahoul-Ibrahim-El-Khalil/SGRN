// ============================================================================
// simulation.as — Single Motor / VFD control loop
//
// This script IS the controlling unit — the VFD/motor-starter firmware. It
// does not simulate a plant around the motor; it simulates the drive itself:
// ramping, current draw, thermal rise, overcurrent/overtemp/stall
// protection, and an e-stop/fault-reset handshake.
//
// The only way to command it is over OPC-UA. The embedded OpcUaServer below
// exposes this script's own memory directly — an external OPC-UA client
// (UaExpert, Python `asyncua`, a real PLC, whatever) connects to
// opc.tcp://<host>:4840 and writes straight into DB1 "MotorCommand" fields
// (start, stop, speed_sp_pct, accel_time_s, ...) exactly as if wiring into
// the drive's control terminals. There's no .get()/.put() round trip on the
// command side: the OPC-UA write lands directly in this VM's memory, and
// this loop reads it on the very next scan.
//
// Run with:   s7shell simulation.as
// ============================================================================

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_IP = "127.0.0.1";
const uint16 GATEWAY_PORT = 102;

S7Client@ plc = null;
OpcUaServer@ opc = null;

// ─── Drive / motor nameplate constants ──────────────────────────────────────
const double SCAN_HZ = 10.0; // 10 scans/sec — smooth ramps
const double DT = 1.0 / SCAN_HZ;

const double RATED_RPM = 1450.0; // 4-pole, 50 Hz induction motor
const double RATED_FREQ_HZ = 50.0;
const double RATED_CURRENT_A = 15.0;
const double NO_LOAD_CURRENT_A = 4.0; // magnetising current, present at any speed > 0
const double LOAD_FACTOR = 0.65; // simulated fixed mechanical load, fraction of rated torque
const double LOCKED_ROTOR_MULT = 3.2; // inrush multiplier shaping the accel current bump

const double OVERCURRENT_TRIP_A = 22.0;
const double OVERCURRENT_TRIP_DELAY_S = 2.0; // must persist this long to trip (thermal-overload-relay style)

const double AMBIENT_C = 25.0;
const double TRIP_TEMP_C = 130.0;
const double RESET_TEMP_C = 100.0; // must cool below this before a fault reset is honoured
const double HEATING_RATE_C_S = 2.0; // deg C/s at rated current squared
const double COOLING_RATE = 0.02; // fraction of (temp - ambient) lost per second

const double STALL_TIMEOUT_S = 5.0; // commanded run, high current, near-zero speed this long => stall
const double STALL_CURRENT_A = RATED_CURRENT_A * 0.5;
const double STALL_SPEED_PCT = 2.0;

const uint16 FAULT_NONE = 0;
const uint16 FAULT_OVERCURRENT = 1;
const uint16 FAULT_OVERTEMP = 2;
const uint16 FAULT_ESTOP = 3;
const uint16 FAULT_STALL = 4;

// ─── Setup ────────────────────────────────────────────────────────────────

bool setupDrive() {
    print("  Motor/VFD controller — connecting to gateway at " + GATEWAY_IP + ":" + GATEWAY_PORT + "\n");

    @plc = S7Client(GATEWAY_IP, 0, 1, GATEWAY_PORT);
    plc.loadSclSchema(SCHEMA_PATH);

    if (!plc.isConnected()) {
        print("  ERROR: could not connect to gateway: " + plc.lastError() + "\n");
        return false;
    }

    @opc = OpcUaServer(plc.runtime(), 4840);
    if (!opc.start()) {
        print("  WARNING: Could not start OPC-UA server on 4840.\n");
    } else {
        print("  OPC-UA control surface live at opc.tcp://" + GATEWAY_IP + ":4840\n");
        print("  Write MotorCommand.start / speed_sp_pct / accel_time_s to drive it.\n");
    }

    print("  Connected. Scan rate: " + SCAN_HZ + " Hz\n");
    return true;
}

// ─── Main scan cycle ─────────────────────────────────────────────────────────

void main() {
    if (!setupDrive())
        return;

    // No .get() here: motor_command is the OPC-UA adapter's own write
    // target (this VM's memory), and motor_status/alarms are what we
    // publish into — there's nothing external to seed from.

    bool run_latch = false;
    bool prev_start = false;
    bool prev_fault_reset = false;

    double speed_fb_pct = 0.0;
    double winding_temp_c = AMBIENT_C;
    double overcurrent_timer_s = 0.0;
    double stall_timer_s = 0.0;
    double runtime_h = 0.0;
    bool fault = false;
    uint16 fault_code = FAULT_NONE;
    bool direction_actual_rev = false;

    int iteration = 0;

    while (true) {
        DTL@ ts = dtl();

        // ── 1. Read commands (written directly by the OPC-UA client) ────────
        bool start_cmd = motor_command.start;
        bool stop_cmd = motor_command.stop;
        bool e_stop = motor_command.e_stop;
        bool fault_reset_cmd = motor_command.fault_reset;
        bool direction_rev_cmd = motor_command.direction_rev;

        double speed_sp_pct = double(motor_command.speed_sp_pct);
        if (speed_sp_pct < 0.0) speed_sp_pct = 0.0;
        if (speed_sp_pct > 100.0) speed_sp_pct = 100.0;

        double accel_time_s = double(motor_command.accel_time_s);
        if (accel_time_s < 0.1) accel_time_s = 0.1;
        double decel_time_s = double(motor_command.decel_time_s);
        if (decel_time_s < 0.1) decel_time_s = 0.1;

        double torque_limit_pct = double(motor_command.torque_limit_pct);
        if (torque_limit_pct <= 0.0) torque_limit_pct = 150.0; // unset => no extra limit

        // ── 2. Start/stop edge logic ─────────────────────────────────────────
        bool start_edge = start_cmd && !prev_start;
        prev_start = start_cmd;

        if (e_stop) {
            run_latch = false;
            if (!fault) {
                fault = true;
                fault_code = FAULT_ESTOP;
            }
        } else if (stop_cmd) {
            run_latch = false;
        } else if (start_edge && !fault) {
            run_latch = true;
        }

        // ── 3. Fault reset handshake — only clears once the root cause is gone ──
        bool fault_reset_edge = fault_reset_cmd && !prev_fault_reset;
        prev_fault_reset = fault_reset_cmd;

        if (fault_reset_edge && fault && !e_stop) {
            bool current_clear = overcurrent_timer_s <= 0.0;
            bool temp_clear = winding_temp_c < RESET_TEMP_C;
            bool stall_clear = stall_timer_s <= 0.0;
            if (current_clear && temp_clear && stall_clear) {
                fault = false;
                fault_code = FAULT_NONE;
            }
        }

        // ── 4. Direction — only allowed to change while essentially stopped ──
        if (speed_fb_pct < 1.0)
            direction_actual_rev = direction_rev_cmd;

        // ── 5. Speed ramp (accel/decel per the commanded ramp times) ────────
        double target_pct = (run_latch && !fault) ? speed_sp_pct : 0.0;

        double accel_rate = 100.0 / accel_time_s; // %/s
        double decel_rate = 100.0 / decel_time_s; // %/s
        // A live e-stop always uses a hard 1-second coast-to-zero, regardless
        // of the configured decel ramp — this models a real hardwired stop
        // category, not a polite ramp-down.
        if (e_stop) decel_rate = 100.0 / 1.0;

        double prev_speed_fb = speed_fb_pct;
        if (speed_fb_pct < target_pct) {
            speed_fb_pct += accel_rate * DT;
            if (speed_fb_pct > target_pct) speed_fb_pct = target_pct;
        } else if (speed_fb_pct > target_pct) {
            speed_fb_pct -= decel_rate * DT;
            if (speed_fb_pct < target_pct) speed_fb_pct = target_pct;
        }
        if (speed_fb_pct < 0.0) speed_fb_pct = 0.0;
        if (speed_fb_pct > 100.0) speed_fb_pct = 100.0;

        bool running = speed_fb_pct > 0.5;
        double accel_rate_actual = (speed_fb_pct - prev_speed_fb) / DT; // signed, %/s

        // ── 6. Current model ──────────────────────────────────────────────
        double speed_ratio = speed_fb_pct / 100.0;
        double base_current = running
            ? NO_LOAD_CURRENT_A + speed_ratio * (RATED_CURRENT_A - NO_LOAD_CURRENT_A) * LOAD_FACTOR
            : 0.0;

        // Inrush/accel current bump: proportional to how hard we're
        // accelerating relative to the maximum configured ramp rate.
        double accel_boost = 0.0;
        if (running && accel_rate_actual > 0.0) {
            double accel_fraction = accel_rate_actual / accel_rate; // 0..1
            if (accel_fraction > 1.0) accel_fraction = 1.0;
            accel_boost = accel_fraction * RATED_CURRENT_A * (LOCKED_ROTOR_MULT - 1.0) * 0.5;
        }

        double current_a = base_current + accel_boost;

        // ── 6b. Torque-limit current foldback (VFD-style current limiting) ──
        double max_current_for_torque = NO_LOAD_CURRENT_A + (torque_limit_pct / 100.0) * (RATED_CURRENT_A - NO_LOAD_CURRENT_A);
        if (current_a > max_current_for_torque)
            current_a = max_current_for_torque;

        // ── 7. Overcurrent protection (thermal-overload-relay style delay) ──
        if (current_a > OVERCURRENT_TRIP_A) {
            overcurrent_timer_s += DT;
            if (overcurrent_timer_s >= OVERCURRENT_TRIP_DELAY_S && !fault) {
                fault = true;
                fault_code = FAULT_OVERCURRENT;
                run_latch = false;
            }
        } else if (overcurrent_timer_s > 0.0) {
            overcurrent_timer_s -= DT * 2.0; // resets faster than it trips
            if (overcurrent_timer_s < 0.0) overcurrent_timer_s = 0.0;
        }

        // ── 8. Thermal model ──────────────────────────────────────────────
        double load_ratio = current_a / RATED_CURRENT_A;
        winding_temp_c += (load_ratio * load_ratio) * HEATING_RATE_C_S * DT;
        winding_temp_c -= (winding_temp_c - AMBIENT_C) * COOLING_RATE * DT;
        if (winding_temp_c < AMBIENT_C) winding_temp_c = AMBIENT_C;

        if (winding_temp_c >= TRIP_TEMP_C && !fault) {
            fault = true;
            fault_code = FAULT_OVERTEMP;
            run_latch = false;
        }

        // ── 9. Stall protection ──────────────────────────────────────────
        if (run_latch && current_a > STALL_CURRENT_A && speed_fb_pct < STALL_SPEED_PCT) {
            stall_timer_s += DT;
            if (stall_timer_s >= STALL_TIMEOUT_S && !fault) {
                fault = true;
                fault_code = FAULT_STALL;
                run_latch = false;
            }
        } else if (stall_timer_s > 0.0) {
            stall_timer_s -= DT * 2.0;
            if (stall_timer_s < 0.0) stall_timer_s = 0.0;
        }

        // ── 10. Derived readouts ────────────────────────────────────────────
        double torque_pct = 0.0;
        if (running) {
            torque_pct = ((current_a - NO_LOAD_CURRENT_A) / (RATED_CURRENT_A - NO_LOAD_CURRENT_A)) * 100.0;
            if (torque_pct < 0.0) torque_pct = 0.0;
            if (torque_pct > 150.0) torque_pct = 150.0;
        }

        double speed_rpm = speed_ratio * RATED_RPM;
        double output_freq_hz = speed_ratio * RATED_FREQ_HZ;

        if (running) runtime_h += DT / 3600.0;

        motor_command.timestamp = dtl();
        motor_command.put();
        // ── 11. Publish status back over shared memory (OPC-UA reads this) ──
        motor_status.drive.running = running;
        motor_status.drive.ready = !fault;
        motor_status.drive.speed_sp_pct = float(speed_sp_pct);
        motor_status.drive.speed_fb_pct = float(speed_fb_pct);
        motor_status.drive.speed_rpm = float(speed_rpm);
        motor_status.drive.output_freq_hz = float(output_freq_hz);
        motor_status.drive.current_a = float(current_a);
        motor_status.drive.torque_pct = float(torque_pct);
        motor_status.drive.winding_temp_c = float(winding_temp_c);
        motor_status.drive.fault = fault;
        motor_status.drive.fault_code = fault_code;
        motor_status.drive.runtime_h = runtime_h;
        motor_status.direction_actual_rev = direction_actual_rev;
        motor_status.timestamp = dtl();
        motor_status.put();

        alarms.any_active = fault || e_stop;
        alarms.overcurrent_trip = (fault_code == FAULT_OVERCURRENT);
        alarms.overtemp_trip = (fault_code == FAULT_OVERTEMP);
        alarms.estop_active = e_stop;
        alarms.stall_trip = (fault_code == FAULT_STALL);
        alarms.timestamp = dtl();
        alarms.put();

        iteration++;
        sleep(int(1000.0 / SCAN_HZ));
    }
}
