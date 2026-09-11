// ============================================================================
// operator.as — "MiniPlant" operator / forcing station (program #3)
//
// This is the third program: another, independent s7shell process. It has
// no knowledge of the PLC's physics — it only connects to the same gateway
// (program #1) as an S7Client and forces values into DB1 "Setpoints". The
// PLC (program #2, plc_logic.as) polls those setpoints every scan and reacts.
//
// Run with:   s7shell operator.as
//
// This script runs an unattended demo scenario. For live, ad-hoc forcing,
// just run `s7shell` with no file and use the REPL commands shown at the
// bottom of this file instead.
// ============================================================================

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_IP = "127.0.0.1";
const uint16 GATEWAY_PORT = 102;

S7Client@ plc = null;

bool setupOperator() {
    print("================================================================\n");
    print("  Operator station — connecting to gateway at " + GATEWAY_IP + ":" + GATEWAY_PORT + "\n");

    @plc = S7Client(GATEWAY_IP, 0, 1, GATEWAY_PORT);
    plc.loadSclSchema(SCHEMA_PATH);

    if (!plc.isConnected()) {
        print("  ERROR: could not connect to gateway: " + plc.lastError() + "\n");
        return false;
    }
    print("  Connected.\n");
    print("================================================================\n");
    return true;
}

void force(bool pump_run, double pump_speed, double inlet_pct, double outlet_pct,
    bool heater_on, double heater_sp, bool e_stop) {
    setpoints.pump_run = pump_run;
    setpoints.pump_speed_sp_pct = float(pump_speed);
    setpoints.inlet_valve_cmd_pct = float(inlet_pct);
    setpoints.outlet_valve_cmd_pct = float(outlet_pct);
    setpoints.heater_enable = heater_on;
    setpoints.heater_setpoint_c = float(heater_sp);
    setpoints.e_stop = e_stop;
    setpoints.timestamp = dtl();
    setpoints.put();
}

void main() {
    if (!setupOperator())
        return;

    print(">>> Step 1: fill the tank — start pump, open inlet valve\n");
    force(true, 70.0, 80.0, 0.0, false, 0.0, false);
    sleep(15000);

    print(">>> Step 2: start heating while filling continues\n");
    force(true, 70.0, 80.0, 0.0, true, 55.0, false);
    sleep(15000);

    print(">>> Step 3: crack open the outlet valve — steady-state flow\n");
    force(true, 70.0, 80.0, 40.0, true, 55.0, false);
    sleep(15000);

    print(">>> Step 4: FORCE EMERGENCY STOP — watch the PLC trip pump + valves\n");
    force(true, 70.0, 80.0, 40.0, true, 55.0, true);
    sleep(8000);

    print(">>> Step 5: reset — release e-stop, drain the tank down\n");
    force(false, 0.0, 0.0, 100.0, false, 0.0, false);
    sleep(15000);

    print(">>> Scenario complete. Setpoints left in a safe, idle state.\n");
}

// ============================================================================
// Ad-hoc forcing from the REPL instead of running this script:
//
// NOTE: this block is deliberately NOT written as literal `plc.loadSclSchema(...)`
// source text. s7shell's pre-scanner finds schema loads with a plain regex
// over the raw file text (it does not understand comments), so writing that
// exact call pattern inside a comment makes it get scanned TWICE and the
// script fails to compile with "function already exists". Type the schema
// load by hand at the prompt instead of pasting it from a comment.
//
//   1. Launch:            s7shell
//   2. Create a client:   S7Client@ plc = S7Client("127.0.0.1", 0, 1, 102);
//   3. Load the schema on that client (method name: load-Scl-Schema),
//      pointing it at "schema.scl" — this is what injects the `setpoints`,
//      `tank`, `pump`, `valves`, `heater`, `alarms` globals into the REPL.
//   4. Then, e.g.:
//        setpoints.pump_run = true;
//        setpoints.pump_speed_sp_pct = 80.0;
//        setpoints.inlet_valve_cmd_pct = 60.0;
//        setpoints.put();
//        setpoints.e_stop = true; setpoints.put();   // trip it
//        tank                                        // bare expr -> auto JSON dump
//        tank.get(); tank.print();
// ============================================================================
