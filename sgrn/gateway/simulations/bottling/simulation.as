const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_IP = "127.0.0.1";
const uint16 GATEWAY_PORT = 102;

S7Client@ plc = null;
OpcUaServer@ opc = null;

// ─── Plant constants ─────────────────────────────────────────────────────────

const double SCAN_HZ = 5.0; // 5 scans/sec
const double DT = 1.0 / SCAN_HZ;

const double LINE1_RATED_BPM = 300.0; // 750 mL glass line
const double LINE2_RATED_BPM = 500.0; // 330 mL PET/can line

const double LINE1_FILL_TARGET_ML = 750.0;
const double LINE2_FILL_TARGET_ML = 330.0;

const double LINE1_TORQUE_TARGET_NM = 3.2;
const double LINE2_TORQUE_TARGET_NM = 1.6;

const double TANK_CAPACITY_L = 20000.0;
const double AIR_HEADER_SP_BAR = 6.8;
const double GLYCOL_SP_C = 2.0;

// ─── Setup ────────────────────────────────────────────────────────────────

bool setupPlant() {
    print("================================================================\n");
    print("  Bottling Plant PLC — connecting to gateway at " + GATEWAY_IP + ":" + GATEWAY_PORT + "\n");

    @plc = S7Client(GATEWAY_IP, 0, 1, GATEWAY_PORT);
    plc.loadSclSchema(SCHEMA_PATH);

    if (!plc.isConnected()) {
        print("  ERROR: could not connect to gateway: " + plc.lastError() + "\n");
        return false;
    }

    @opc = OpcUaServer(plc.runtime(), 4840);
    if (!opc.start()) {
        print("  WARNING: Could not start OPC-UA server on 4840.\n");
    }

    print("  Connected. Scan rate: " + SCAN_HZ + " Hz\n");
    print("================================================================\n");
    return true;
}

// ─── Generic helpers ─────────────────────────────────────────────────────────

// Ramp a MotorDrive towards a target speed (in % of rated) at a fixed rate.
void driveMotor(MotorDrive@ m, double target_pct, double dt, double ramp_pct_per_s = 60.0) {
    double sp = double(m.speed_fb);
    double step = ramp_pct_per_s * dt;
    if (sp < target_pct) { sp += step; if (sp > target_pct) sp = target_pct; } else if (sp > target_pct) { sp -= step; if (sp < target_pct) sp = target_pct; }
    if (sp < 0.0) sp = 0.0;
    if (sp > 100.0) sp = 100.0;
    m.speed_sp = float(target_pct);
    m.speed_fb = float(sp);
    m.running = sp > 0.5;
    if (m.running) m.runtime += dt / 3600.0;
    m.current = float(5.0 + (sp / 100.0) * 18.0 + (m.fault ? 0.0 : 0.0));
}

void driveValve(ValveActuator@ v, double dt) {
    if (v.travel_time <= 0.0f) v.travel_time = 2.0f;
    double rate = 100.0 / double(v.travel_time);
    double pos = double(v.position);
    double cmd = double(v.command);
    double step = rate * dt;
    if (pos < cmd) { pos += step; if (pos > cmd) pos = cmd; } else if (pos > cmd) { pos -= step; if (pos < cmd) pos = cmd; }
    if (pos < 0.0) pos = 0.0;
    if (pos > 100.0) pos = 100.0;
    v.position = float(pos);
    v.limit_open = (pos >= 99.0);
    v.limit_closed = (pos <= 1.0);
}

// Trim factor (0..1) for a station feeding INTO a buffer: throttles back as
// the downstream buffer approaches full so it doesn't overrun the next
// machine.
double trimFeeding(double buffer_fill) {
    double f = 1.0 - (buffer_fill - 80.0) / 20.0;
    if (f > 1.0) f = 1.0;
    if (f < 0.0) f = 0.0;
    return f;
}

// Trim factor (0..1) for a station DRAWING FROM a buffer: throttles back as
// the upstream buffer runs dry (starved).
double trimDrawing(double buffer_fill) {
    double f = (buffer_fill - 5.0) / 15.0;
    if (f > 1.0) f = 1.0;
    if (f < 0.0) f = 0.0;
    return f;
}

// Update an accumulation zone's fill level given the produce/consume rates
// (bottles/min) of the machines on either side of it. zone_capacity_bottles
// is the physical buffer size (accumulation table / conveyor length).
void updateZone(AccumulationZone@ zone, double produce_bpm, double consume_bpm, double dt,
    double zone_capacity_bottles) {
    double delta_bottles = (produce_bpm - consume_bpm) * (dt / 60.0);
    double delta_pct = (delta_bottles / zone_capacity_bottles) * 100.0;
    double fill = double(zone.fill) + delta_pct;
    if (fill < 0.0) fill = 0.0;
    if (fill > 100.0) fill = 100.0;
    zone.fill = float(fill);
    zone.upstream_starve = fill <= 2.0;
    zone.downstream_block = fill >= 98.0;
}
double fmax(double t_x, double t_y) {
    if(t_x >= t_y) {
        return t_x;
    }
    return t_y;
}
// ─── Main scan cycle ─────────────────────────────────────────────────────────

void main() {
    if (!setupPlant())
        return;

    // No seed .get() here: `supervisor`, `product_supply`, etc. are handles
    // directly into the s7shell VM's own memory (the same memory the
    // embedded OpcUaServer writes into) — they already hold whatever state
    // exists, there's nothing external to fetch before we start.

    double tank_l = double(product_supply.tank.volume) > 0.0 ? double(product_supply.tank.volume) : TANK_CAPACITY_L * 0.7;
    double air_press = 6.8;
    double glycol_supply_c = 3.0;

    int iteration = 0;

    while (true) {
        DTL@ ts = dtl();

        // ── 1. Read current state ────────────────────────────────────────────
        // No .get() here either: the OPC-UA adapter writes setpoints
        // (master_speed_sp, plant_mode, reset_request, estop_zones[i], ...)
        // straight into this same VM memory, so `supervisor` and
        // `safety_systems` already reflect the latest values. `.get()` would
        // imply pulling from the gateway, but the gateway is a passive
        // receiver that never originates data — there'd be nothing to pull.

        // ── 2. Plant safety propagation — critical, pushed immediately ──────
        bool any_estop = false;
        for (uint i = 0; i < 8; i++) {
            if (safety_systems.estop_zones[i] || safety_systems.guard_door_open[i]) any_estop = true;
        }
        for (uint i = 0; i < 4; i++) {
            if (safety_systems.light_curtain_broken[i]) any_estop = true;
        }
        safety_systems.any_estop_active = any_estop;
        safety_systems.reset_interlock_ok = !any_estop;
        safety_systems.timestamp = ts;
        safety_systems.put(); // flush now — don't wait on the rest of the scan

        bool plant_estop = supervisor.plant_estop || any_estop;
        supervisor.plant_estop = plant_estop;
        if (!plant_estop && supervisor.reset_request) {
            supervisor.plant_mode = 1; // Starting
        }
        supervisor.plant_mode = plant_estop ? 5 : (supervisor.plant_mode == 0 ? 0 : 2);
        supervisor.line1.mode = supervisor.plant_mode;
        supervisor.line2.mode = supervisor.plant_mode;
        supervisor.timestamp = ts;
        supervisor.put(); // flush mode/estop now; speed & counts below flush at step 6

        bool plant_running = (supervisor.plant_mode == 2) && !plant_estop;

        // ── 3. Master speed cascade ─────────────────────────────────────────
        double master_pct = plant_running ? double(supervisor.master_speed_sp) : 0.0;
        if (master_pct < 0.0) master_pct = 0.0;
        if (master_pct > 100.0) master_pct = 100.0;

        double line1_sp_bpm = LINE1_RATED_BPM * (master_pct / 100.0);
        double line2_sp_bpm = LINE2_RATED_BPM * (master_pct / 100.0);
        supervisor.master_speed_sp = float(master_pct);
        supervisor.line1.speed_sp = float(line1_sp_bpm);
        supervisor.line2.speed_sp = float(line2_sp_bpm);
        supervisor.line1.rated = float(LINE1_RATED_BPM);
        supervisor.line2.rated = float(LINE2_RATED_BPM);

        // ── 4. Shared product supply / CIP skid ─────────────────────────────
        double consumption_lpm =
            (double(line1_filler.carousel.speed_fb) / 100.0 * line1_sp_bpm * LINE1_FILL_TARGET_ML / 1000.0) +
                (double(line2_filler.carousel.speed_fb) / 100.0 * line2_sp_bpm * LINE2_FILL_TARGET_ML / 1000.0);

        bool cip_running = product_supply.cip.active;
        double makeup_lpm = (!cip_running && tank_l / TANK_CAPACITY_L < 0.85) ? 400.0 : 0.0;
        tank_l += (makeup_lpm - (cip_running ? 0.0 : consumption_lpm)) * DT / 60.0;
        if (tank_l < 0.0) tank_l = 0.0;
        if (tank_l > TANK_CAPACITY_L) tank_l = TANK_CAPACITY_L;

        product_supply.tank.capacity = float(TANK_CAPACITY_L);
        product_supply.tank.volume = float(tank_l);
        product_supply.tank.level = float(tank_l / TANK_CAPACITY_L * 100.0);
        product_supply.tank.low_level_alarm = (tank_l / TANK_CAPACITY_L * 100.0) < 15.0;
        product_supply.tank.temp = 4.0f; // chilled product, held near constant
        product_supply.tank.agitator.running = !cip_running;
        driveMotor(product_supply.tank.agitator, product_supply.tank.agitator.running ? 40.0 : 0.0, DT);
        driveMotor(product_supply.supply_pump, cip_running ? 0.0 : 70.0, DT);

        product_supply.line1_supply.valve.command = (plant_running && !cip_running) ? 100.0f : 0.0f;
        product_supply.line2_supply.valve.command = (plant_running && !cip_running) ? 100.0f : 0.0f;
        driveValve(product_supply.line1_supply.valve, DT);
        driveValve(product_supply.line2_supply.valve, DT);
        product_supply.line1_supply.press = float(double(product_supply.line1_supply.valve.position) / 100.0 * 2.4);
        product_supply.line2_supply.press = float(double(product_supply.line2_supply.valve.position) / 100.0 * 2.4);

        // CIP sequencer (only runs when the OPC-UA client sets cip.active and
        // plant is not running product)
        if (cip_running) {
            product_supply.cip.time_in_state += float(DT);
            float dwell = product_supply.cip.time_in_state;
            uint8 st = product_supply.cip.cycle_state;
            const float STAGE_S = 30.0f; // shortened for simulation purposes
            if (st == 0) { product_supply.cip.cycle_state = 1; product_supply.cip.time_in_state = 0.0f; } else if (dwell > STAGE_S && st < 6) {
                product_supply.cip.cycle_state = st + 1;
                product_supply.cip.time_in_state = 0.0f;
                if (st + 1 == 6) {
                    product_supply.cip.cycles_completed = product_supply.cip.cycles_completed + 1;
                    product_supply.cip.active = false;
                }
            }
            product_supply.cip.supply_temp = (st == 2) ? 75.0f : (st == 4) ? 60.0f : 20.0f;
            product_supply.cip.caustic_conc = (st == 2) ? 2.0f : 0.0f;
            product_supply.cip.supply_flow = 250.0f;
            product_supply.cip.return_conductivity = (st == 2) ? 45.0f : (st >= 3) ? 5.0f : 1.0f;
        } else {
            product_supply.cip.cycle_state = 0;
        }

        // ── 5. Line 1 chain ──────────────────────────────────────────────────
        double l1_avail = plant_running ? 1.0 : 0.0;

        // Infeed
        line1_infeed.infeed_starved = false; // depalletizer assumed well-stocked
        double l1_infeed_bpm = line1_sp_bpm * l1_avail;
        driveMotor(line1_infeed.unscrambler, l1_infeed_bpm / LINE1_RATED_BPM * 100.0, DT);
        line1_infeed.bottles_staged = uint16(double(line1_infeed.table_fill) / 100.0 * 400.0);
        line1_infeed.table_fill = float(70.0); // fed by depalletizer, assumed regulated upstream

        // Rinser (paces to filler bowl buffer)
        double l1_rinse_trim = trimFeeding(double(conveyor_network.line1.rinser_to_filler.fill));
        double l1_rinser_bpm = l1_infeed_bpm * l1_rinse_trim;
        driveMotor(line1_rinser.rinser_turret, l1_rinser_bpm / LINE1_RATED_BPM * 100.0, DT);
        line1_rinser.water_valve.command = plant_running ? 100.0f : 0.0f;
        line1_rinser.air_valve.command = plant_running ? 100.0f : 0.0f;
        driveValve(line1_rinser.water_valve, DT);
        driveValve(line1_rinser.air_valve, DT);
        line1_rinser.rinse_water_press = float(double(line1_rinser.water_valve.position) / 100.0 * 3.0);
        line1_rinser.rinse_air_press = float(double(line1_rinser.air_valve.position) / 100.0 * 5.5);
        line1_rinser.nozzles_total = 48;

        updateZone(conveyor_network.line1.rinser_to_filler, l1_rinser_bpm, 0.0, DT, 60.0);

        // Filler (the pacing/bottleneck station — draws from rinser buffer,
        // trimmed by both the rinser buffer starvation and the downstream
        // capper buffer backing up)
        double l1_filler_draw_trim = trimDrawing(double(conveyor_network.line1.rinser_to_filler.fill));
        double l1_filler_push_trim = trimFeeding(double(conveyor_network.line1.filler_to_capper.fill));
        double l1_filler_bpm = line1_sp_bpm * l1_filler_draw_trim * l1_filler_push_trim;
        driveMotor(line1_filler.carousel, l1_filler_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line1.rinser_to_filler, 0.0, l1_filler_bpm, DT, 60.0); // consume side

        line1_filler.valve_count = 40;
        line1_filler.valves_open_now = uint16(l1_filler_bpm / LINE1_RATED_BPM * 40.0);
        line1_filler.fill_volume.sp = float(LINE1_FILL_TARGET_ML);
        line1_filler.bowl_level.setpoint = 65.0;
        line1_filler.bowl_level.process_value = 65.0 + (product_supply.tank.level < 20.0 ? -8.0 : 0.0);
        line1_filler.bowl_level.enabled = plant_running;
        double l1_bowl_err = double(line1_filler.bowl_level.setpoint) - double(line1_filler.bowl_level.process_value);
        line1_filler.bowl_level.output = float(50.0 + l1_bowl_err * 3.0);
        line1_filler.product_temp = 4.2f;
        line1_filler.co2_volumes = 0.0f; // still product
        double l1_fill_noise = (double(iteration % 7) - 3.0) * 0.15; // deterministic pseudo-variance
        line1_filler.fill_volume.avg = float(LINE1_FILL_TARGET_ML + l1_fill_noise);
        line1_filler.fill_volume.stddev = 0.9f;
        line1_filler.vacuum_snift_press = -0.35f;

        updateZone(conveyor_network.line1.filler_to_capper, l1_filler_bpm, 0.0, DT, 80.0);

        // Capper
        double l1_capper_draw_trim = trimDrawing(double(conveyor_network.line1.filler_to_capper.fill));
        double l1_capper_push_trim = trimFeeding(double(conveyor_network.line1.capper_to_labeler.fill));
        double l1_capper_bpm = line1_sp_bpm * l1_capper_draw_trim * l1_capper_push_trim;
        driveMotor(line1_capper.capper_turret, l1_capper_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line1.filler_to_capper, 0.0, l1_capper_bpm, DT, 80.0);

        line1_capper.chuck_heads = 12;
        line1_capper.cap_feeder_bowl_level = 55.0f;
        line1_capper.cap_feeder_starved = false;
        line1_capper.torque.sp = float(LINE1_TORQUE_TARGET_NM);
        line1_capper.torque.avg = float(LINE1_TORQUE_TARGET_NM + (double(iteration % 5) - 2.0) * 0.05);
        line1_capper.torque.stddev = 0.12f;

        updateZone(conveyor_network.line1.capper_to_labeler, l1_capper_bpm, 0.0, DT, 80.0);

        // Labeler
        double l1_label_draw_trim = trimDrawing(double(conveyor_network.line1.capper_to_labeler.fill));
        double l1_label_push_trim = trimFeeding(double(conveyor_network.line1.labeler_to_packer.fill));
        double l1_labeler_bpm = line1_sp_bpm * l1_label_draw_trim * l1_label_push_trim;
        driveMotor(line1_labeler.labeler_turret, l1_labeler_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line1.capper_to_labeler, 0.0, l1_labeler_bpm, DT, 80.0);

        double l1_label_use = l1_labeler_bpm * DT / 60.0 * 0.0008; // % roll consumed per bottle, scaled
        line1_labeler.label_supply.front = float(fmax(0.0, double(line1_labeler.label_supply.front) - l1_label_use));
        line1_labeler.label_supply.back = float(fmax(0.0, double(line1_labeler.label_supply.back) - l1_label_use));
        line1_labeler.applicator_pressure = 2.1f;

        updateZone(conveyor_network.line1.labeler_to_packer, l1_labeler_bpm, 0.0, DT, 100.0);

        // Case packer
        double l1_pack_draw_trim = trimDrawing(double(conveyor_network.line1.labeler_to_packer.fill));
        double l1_packer_bpm = line1_sp_bpm * l1_pack_draw_trim;
        driveMotor(line1_case_packer.erector, l1_packer_bpm / LINE1_RATED_BPM * 100.0, DT);
        driveMotor(line1_case_packer.packer, l1_packer_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line1.labeler_to_packer, 0.0, l1_packer_bpm, DT, 100.0);

        line1_case_packer.bottles_per_case = 12;
        line1_case_packer.glue.tank_temp.setpoint = 165.0;
        line1_case_packer.glue.tank_temp.process_value = 165.0 - (plant_running ? 0.0 : 20.0);
        line1_case_packer.glue.tank_temp.enabled = true;
        line1_case_packer.glue.pressure = 4.5f;
        line1_case_packer.case_blank.hopper = 60.0f;
        double l1_cases_this_scan = l1_packer_bpm * DT / 60.0 / 12.0;
        line1_case_packer.cases_packed_count += uint32(l1_cases_this_scan);
        supervisor.line1.good_count += uint32(l1_packer_bpm * DT / 60.0);
        supervisor.line1.actual = float(l1_packer_bpm);
        supervisor.line1.oee = float(line1_sp_bpm > 0.0 ? (l1_packer_bpm / line1_sp_bpm * 100.0) : 0.0);

        // ── 6. Line 2 chain (simplified — no rinser, PET/can line) ──────────
        double l2_avail = plant_running ? 1.0 : 0.0;
        double l2_infeed_bpm = line2_sp_bpm * l2_avail;
        driveMotor(line2_infeed.unscrambler, l2_infeed_bpm / LINE2_RATED_BPM * 100.0, DT);
        line2_infeed.infeed_starved = false;
        line2_infeed.table_fill = 70.0f;

        double l2_filler_draw_trim = 1.0; // infeed assumed non-limiting for Line 2
        double l2_filler_push_trim = trimFeeding(double(conveyor_network.line2.filler_to_capper.fill));
        double l2_filler_bpm = line2_sp_bpm * l2_filler_draw_trim * l2_filler_push_trim;
        driveMotor(line2_filler.carousel, l2_filler_bpm / LINE2_RATED_BPM * 100.0, DT);

        line2_filler.valve_count = 24;
        line2_filler.valves_open_now = uint16(l2_filler_bpm / LINE2_RATED_BPM * 24.0);
        line2_filler.fill_volume.sp = float(LINE2_FILL_TARGET_ML);
        line2_filler.bowl_level.setpoint = 65.0;
        line2_filler.bowl_level.process_value = 65.0 + (product_supply.tank.level < 20.0 ? -8.0 : 0.0);
        line2_filler.bowl_level.enabled = plant_running;
        line2_filler.product_temp = 4.2f;
        line2_filler.co2_volumes = 3.8f; // carbonated
        double l2_fill_noise = (double(iteration % 9) - 4.0) * 0.1;
        line2_filler.fill_volume.avg = float(LINE2_FILL_TARGET_ML + l2_fill_noise);
        line2_filler.fill_volume.stddev = 0.6f;
        line2_filler.vacuum_snift_press = -0.3f;

        updateZone(conveyor_network.line2.filler_to_capper, l2_filler_bpm, 0.0, DT, 100.0);

        double l2_capper_draw_trim = trimDrawing(double(conveyor_network.line2.filler_to_capper.fill));
        double l2_capper_push_trim = trimFeeding(double(conveyor_network.line2.capper_to_labeler.fill));
        double l2_capper_bpm = line2_sp_bpm * l2_capper_draw_trim * l2_capper_push_trim;
        driveMotor(line2_capper.capper_turret, l2_capper_bpm / LINE2_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line2.filler_to_capper, 0.0, l2_capper_bpm, DT, 100.0);

        line2_capper.chuck_heads = 8;
        line2_capper.torque.sp = float(LINE2_TORQUE_TARGET_NM);
        line2_capper.torque.avg = float(LINE2_TORQUE_TARGET_NM + (double(iteration % 5) - 2.0) * 0.03);
        line2_capper.torque.stddev = 0.08f;

        updateZone(conveyor_network.line2.capper_to_labeler, l2_capper_bpm, 0.0, DT, 100.0);

        double l2_label_draw_trim = trimDrawing(double(conveyor_network.line2.capper_to_labeler.fill));
        double l2_label_push_trim = trimFeeding(double(conveyor_network.line2.labeler_to_packer.fill));
        double l2_labeler_bpm = line2_sp_bpm * l2_label_draw_trim * l2_label_push_trim;
        driveMotor(line2_labeler.labeler_turret, l2_labeler_bpm / LINE2_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line2.capper_to_labeler, 0.0, l2_labeler_bpm, DT, 100.0);
        line2_labeler.label_supply.front = float(fmax(0.0, double(line2_labeler.label_supply.front) - l2_labeler_bpm * DT / 60.0 * 0.0006));
        line2_labeler.label_supply.back = line2_labeler.label_supply.front;
        line2_labeler.applicator_pressure = 1.8f;

        updateZone(conveyor_network.line2.labeler_to_packer, l2_labeler_bpm, 0.0, DT, 120.0);

        double l2_pack_draw_trim = trimDrawing(double(conveyor_network.line2.labeler_to_packer.fill));
        double l2_packer_bpm = line2_sp_bpm * l2_pack_draw_trim;
        driveMotor(line2_case_packer.erector, l2_packer_bpm / LINE2_RATED_BPM * 100.0, DT);
        driveMotor(line2_case_packer.packer, l2_packer_bpm / LINE2_RATED_BPM * 100.0, DT);
        updateZone(conveyor_network.line2.labeler_to_packer, 0.0, l2_packer_bpm, DT, 120.0);

        line2_case_packer.bottles_per_case = 24;
        line2_case_packer.glue.tank_temp.setpoint = 165.0;
        line2_case_packer.glue.tank_temp.process_value = 165.0 - (plant_running ? 0.0 : 20.0);
        line2_case_packer.glue.pressure = 4.3f;
        line2_case_packer.case_blank.hopper = 60.0f;
        double l2_cases_this_scan = l2_packer_bpm * DT / 60.0 / 24.0;
        line2_case_packer.cases_packed_count += uint32(l2_cases_this_scan);
        supervisor.line2.good_count += uint32(l2_packer_bpm * DT / 60.0);
        supervisor.line2.actual = float(l2_packer_bpm);
        supervisor.line2.oee = float(line2_sp_bpm > 0.0 ? (l2_packer_bpm / line2_sp_bpm * 100.0) : 0.0);

        // ── 7. Merge conveyor + shared palletizer ────────────────────────────
        double merge_in_cases_min = l1_cases_this_scan / (DT / 60.0) + l2_cases_this_scan / (DT / 60.0);
        driveMotor(conveyor_network.merge.conveyor, merge_in_cases_min > 0.1 ? 80.0 : 0.0, DT);
        conveyor_network.merge.line1_gate.command = 100.0f;
        conveyor_network.merge.line2_gate.command = 100.0f;
        driveValve(conveyor_network.merge.line1_gate, DT);
        driveValve(conveyor_network.merge.line2_gate, DT);
        updateZone(conveyor_network.merge.zone, merge_in_cases_min, 0.0, DT, 40.0);

        double palletizer_trim = trimDrawing(double(conveyor_network.merge.zone.fill));
        double palletizer_cases_min = merge_in_cases_min * palletizer_trim;
        updateZone(conveyor_network.merge.zone, 0.0, palletizer_cases_min, DT, 40.0);

        bool robot_active = plant_running && palletizer_cases_min > 0.05;
        palletizer.robot.running = robot_active;
        palletizer.cases_per_layer = 10;
        palletizer.layers_per_pallet = 6;
        double cases_this_scan = palletizer_cases_min * DT / 60.0;
        int total_case_slot = int(double(palletizer.current_pallet_case_count) + cases_this_scan);
        palletizer.current_pallet_case_count = uint16(total_case_slot % 60);
        palletizer.current_layer = uint16((total_case_slot % 60) / 10);
        palletizer.pallet_height = float(150.0 + double(palletizer.current_layer) * 220.0);
        palletizer.robot.cycle_state = robot_active ? uint8(1) : uint8(0);

        bool pallet_complete = total_case_slot >= 60;
        if (pallet_complete) {
            palletizer.pallets_completed_count = palletizer.pallets_completed_count + 1;
            palletizer.wrapper.active = true;
            palletizer.wrapper.turntable_speed = 8.0f;
            palletizer.wrapper.film_tension = 35.0f;
            palletizer.wrapper.wraps_target = 4;
            palletizer.wrapper.wraps_completed = 4;
        } else {
            palletizer.wrapper.active = false;
            palletizer.wrapper.turntable_speed = 0.0f;
        }
        driveMotor(palletizer.full_pallet_conveyor, pallet_complete ? 50.0 : 0.0, DT);
        palletizer.empty_pallet_dispenser_starved = false;

        // ── 8. Plant utilities ───────────────────────────────────────────────
        double air_demand = plant_running ? 1.0 : 0.3;
        air_press += ((AIR_HEADER_SP_BAR - air_press) * 0.3 - air_demand * 0.15) * DT;
        if (air_press < 0.0) air_press = 0.0;
        utilities.air.header_press_sp = float(AIR_HEADER_SP_BAR);
        utilities.air.header_press = float(air_press);
        utilities.air.low_press_alarm = air_press < 5.5;
        driveMotor(utilities.air.compressor_1, 70.0, DT);
        driveMotor(utilities.air.compressor_2, air_press < 6.5 ? 70.0 : 0.0, DT);
        utilities.air.dryer_dewpoint = -40.0f;

        glycol_supply_c += ((GLYCOL_SP_C - glycol_supply_c) * 0.2) * DT;
        utilities.glycol.supply_temp.setpoint = GLYCOL_SP_C;
        utilities.glycol.supply_temp.process_value = glycol_supply_c;
        utilities.glycol.supply_temp.enabled = true;
        utilities.glycol.return_temp = float(glycol_supply_c + 3.5);
        driveMotor(utilities.glycol.chiller_compressor, 65.0, DT);
        driveMotor(utilities.glycol.pump, 60.0, DT);

        utilities.vacuum.header = -68.0f;
        driveMotor(utilities.vacuum.pump, plant_running ? 55.0 : 0.0, DT);

        // ── 9. Alarm aggregation — critical, pushed immediately ─────────────
        uint alarm_count = 0;
        if (product_supply.tank.low_level_alarm) alarm_count++;
        if (utilities.air.low_press_alarm) alarm_count++;
        if (safety_systems.any_estop_active) alarm_count++;
        if (line1_infeed.jam_detected) alarm_count++;
        if (line2_infeed.jam_detected) alarm_count++;

        alarms.active_count = uint16(alarm_count);
        alarms.any_active = alarm_count > 0;
        alarms.plant_estop_active = plant_estop;
        alarms.line1_fault_active = supervisor.line1.mode == 4;
        alarms.line2_fault_active = supervisor.line2.mode == 4;
        alarms.utilities_fault_active = utilities.air.low_press_alarm;
        alarms.any_critical = plant_estop;
        alarms.critical_count = plant_estop ? 1 : 0;
        alarms.timestamp = ts;
        alarms.put(); // flush now — don't wait behind the routine telemetry below

        // ── 10. Publish routine telemetry ────────────────────────────────────
        // SafetySystems, Supervisor and Alarms were already flushed above
        // (steps 2 and 9) the moment their critical fields were computed.
        // Everything else is routine process telemetry — dirty tracking
        // means these calls are cheap even though they run every scan; only
        // fields that actually moved since the last .put() go out on the wire.
        supervisor.timestamp = ts;
        supervisor.put(); // flushes speed_sp/oee/counts set since step 2
        product_supply.timestamp = ts;
        product_supply.put();
        line1_infeed.timestamp = ts;
        line1_infeed.put();
        line1_rinser.timestamp = ts;
        line1_rinser.put();
        line1_filler.timestamp = ts;
        line1_filler.put();
        line1_capper.timestamp = ts;
        line1_capper.put();
        line1_labeler.timestamp = ts;
        line1_labeler.put();
        line1_case_packer.timestamp = ts;
        line1_case_packer.put();
        line2_infeed.timestamp = ts;
        line2_infeed.put();
        line2_filler.timestamp = ts;
        line2_filler.put();
        line2_capper.timestamp = ts;
        line2_capper.put();
        line2_labeler.timestamp = ts;
        line2_labeler.put();
        line2_case_packer.timestamp = ts;
        line2_case_packer.put();
        conveyor_network.timestamp = ts;
        conveyor_network.put();
        palletizer.timestamp = ts;
        palletizer.put();
        utilities.timestamp = ts;
        utilities.put();

        iteration++;
        sleep(int(1000.0 / SCAN_HZ));
    }
}
