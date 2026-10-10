// ============================================================================
// simulation.as — Single Motor / VFD control loop
//
// This script IS the controlling unit — the VFD/motor-starter firmware. It
// does not simulate a plant around the motor; it simulates the drive itself:
// ramping, current draw, thermal rise, overcurrent/overtemp/stall
// protection, and an e-stop/fault-reset handshake.
//
// The script owns its virtual PLC memory and synchronizes it with the gateway
// over RuntimeSync WebSocket. Gateway clients can write commands into the
// same state and observe the simulated drive outputs.
//
// Run with:   s7shell simulation.as
// ============================================================================

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";

PlcRuntime@ plc = PlcRuntime(SCHEMA_PATH);
GatewaySync@ sync = null;

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
    print("Motor/VFD controller — connecting runtime to " + GATEWAY_WS_URL + "\n");
    @sync = GatewaySync(plc);
    sync.useBinary(true);
    sync.publishOnDirty(true);
    if (!sync.connect(GATEWAY_WS_URL)) {
        print("ERROR: could not start RuntimeSync: " + sync.lastError() + "\n");
        return false;
    }

    print("Connected. Scan rate: " + SCAN_HZ + " Hz\n");
    return true;
}

// ─── Main scan cycle ─────────────────────────────────────────────────────────

void main() {
    if (!setupDrive())
        return;

    // Gateway commands arrive through RuntimeSync subscriptions.

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
        bool start_cmd = MotorCommand.start;
        bool stop_cmd = MotorCommand.stop;
        bool e_stop = MotorCommand.e_stop;
        bool fault_reset_cmd = MotorCommand.fault_reset;
        bool direction_rev_cmd = MotorCommand.direction_rev;

        double speed_sp_pct = double(MotorCommand.speed_sp_pct);
        if (speed_sp_pct < 0.0) speed_sp_pct = 0.0;
        if (speed_sp_pct > 100.0) speed_sp_pct = 100.0;

        double accel_time_s = double(MotorCommand.accel_time_s);
        if (accel_time_s < 0.1) accel_time_s = 0.1;
        double decel_time_s = double(MotorCommand.decel_time_s);
        if (decel_time_s < 0.1) decel_time_s = 0.1;

        double torque_limit_pct = double(MotorCommand.torque_limit_pct);
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

        MotorCommand.timestamp = dtl();
        MotorCommand.put();
        // ── 11. Publish status back over shared memory (OPC-UA reads this) ──
        MotorStatus.drive.running = running;
        MotorStatus.drive.ready = !fault;
        MotorStatus.drive.speed_sp_pct = float(speed_sp_pct);
        MotorStatus.drive.speed_fb_pct = float(speed_fb_pct);
        MotorStatus.drive.speed_rpm = float(speed_rpm);
        MotorStatus.drive.output_freq_hz = float(output_freq_hz);
        MotorStatus.drive.current_a = float(current_a);
        MotorStatus.drive.torque_pct = float(torque_pct);
        MotorStatus.drive.winding_temp_c = float(winding_temp_c);
        MotorStatus.drive.fault = fault;
        MotorStatus.drive.fault_code = fault_code;
        MotorStatus.drive.runtime_h = runtime_h;
        MotorStatus.direction_actual_rev = direction_actual_rev;
        MotorStatus.timestamp = dtl();
        MotorStatus.put();

        Alarms.any_active = fault || e_stop;
        Alarms.overcurrent_trip = (fault_code == FAULT_OVERCURRENT);
        Alarms.overtemp_trip = (fault_code == FAULT_OVERTEMP);
        Alarms.estop_active = e_stop;
        Alarms.stall_trip = (fault_code == FAULT_STALL);
        Alarms.timestamp = dtl();
        Alarms.put();

        iteration++;
        sleep(int(1000.0 / SCAN_HZ));
    }
}
