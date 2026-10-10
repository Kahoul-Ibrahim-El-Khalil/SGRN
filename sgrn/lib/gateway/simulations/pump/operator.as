// ============================================================================
// operator.as — "MiniPlant" operator / forcing station (program #3)
//
// This operator process owns a typed runtime and sends Setpoints updates to
// the gateway over RuntimeSync WebSocket. The simulation process receives
// those writes through its own RuntimeSync connection.
//
// Run with:   s7shell operator.as
//
// This script runs an unattended demo scenario. For live, ad-hoc forcing,
// just run `s7shell` with no file and use the REPL commands shown at the
// bottom of this file instead.
// ============================================================================

const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";

PlcRuntime@ plc = PlcRuntime(SCHEMA_PATH);
GatewaySync@ sync = null;

bool setupOperator() {
    print("================================================================\n");
    print("  Operator station — connecting to gateway at " + GATEWAY_WS_URL + "\n");
    @sync = GatewaySync(plc);
    sync.useBinary(true);
    sync.publishOnDirty(true);
    if (!sync.connect(GATEWAY_WS_URL)) {
        print("  ERROR: RuntimeSync could not start: " + sync.lastError() + "\n");
        return false;
    }
    print("  RuntimeSync started.\n");
    print("================================================================\n");
    return true;
}

void force(bool pump_run, double pump_speed, double inlet_pct, double outlet_pct,
    bool heater_on, double heater_sp, bool e_stop) {
    Setpoints.pump_run = pump_run;
    Setpoints.pump_speed_sp_pct = float(pump_speed);
    Setpoints.inlet_valve_cmd_pct = float(inlet_pct);
    Setpoints.outlet_valve_cmd_pct = float(outlet_pct);
    Setpoints.heater_enable = heater_on;
    Setpoints.heater_setpoint_c = float(heater_sp);
    Setpoints.e_stop = e_stop;
    Setpoints.timestamp = dtl();
    Setpoints.put();
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
// Interactive use: create PlcRuntime("schema.scl"), then connect a
// GatewaySync instance to the gateway WebSocket before writing fields.
//        Setpoints.pump_run = true;
//        Setpoints.pump_speed_sp_pct = 80.0;
//        Setpoints.inlet_valve_cmd_pct = 60.0;
//        Setpoints.put();
//        Setpoints.e_stop = true; Setpoints.put();   // trip it
//        Tank                                        // bare expr -> auto JSON dump
//        GatewaySync@ sync = GatewaySync(plc);
//        sync.useBinary(true); sync.publishOnDirty(true);
//        sync.connect("ws://127.0.0.1:8000/ws");
//        Tank.print();
// ============================================================================
