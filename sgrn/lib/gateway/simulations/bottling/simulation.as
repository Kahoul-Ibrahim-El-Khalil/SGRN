const string SCHEMA_PATH = "schema.scl";
const string GATEWAY_WS_URL = "ws://127.0.0.1:8000/ws";

PlcRuntime@ plc = PlcRuntime(SCHEMA_PATH);
GatewaySync@ sync = null;

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
    print("  Bottling simulation — connecting to gateway at " + GATEWAY_WS_URL + "\n");
    @sync = GatewaySync(plc);
    sync.useBinary(true);
    sync.publishOnDirty(true);
    if (!sync.connect(GATEWAY_WS_URL)) {
        print("  ERROR: RuntimeSync could not start: " + sync.lastError() + "\n");
        return false;
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

    double tank_l = double(ProductSupply.tank.volume) > 0.0 ? double(ProductSupply.tank.volume) : TANK_CAPACITY_L * 0.7;
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
            if (SafetySystems.estop_zones[i] || SafetySystems.guard_door_open[i]) any_estop = true;
        }
        for (uint i = 0; i < 4; i++) {
            if (SafetySystems.light_curtain_broken[i]) any_estop = true;
        }
        SafetySystems.any_estop_active = any_estop;
        SafetySystems.reset_interlock_ok = !any_estop;
        SafetySystems.timestamp = ts;
        SafetySystems.put(); // flush now — don't wait on the rest of the scan

        bool plant_estop = Supervisor.plant_estop || any_estop;
        Supervisor.plant_estop = plant_estop;
        if (!plant_estop && Supervisor.reset_request) {
            Supervisor.plant_mode = 1; // Starting
        }
        Supervisor.plant_mode = plant_estop ? 5 : (Supervisor.plant_mode == 0 ? 0 : 2);
        Supervisor.line1.mode = Supervisor.plant_mode;
        Supervisor.line2.mode = Supervisor.plant_mode;
        Supervisor.timestamp = ts;
        Supervisor.put(); // flush mode/estop now; speed & counts below flush at step 6

        bool plant_running = (Supervisor.plant_mode == 2) && !plant_estop;

        // ── 3. Master speed cascade ─────────────────────────────────────────
        double master_pct = plant_running ? double(Supervisor.master_speed_sp) : 0.0;
        if (master_pct < 0.0) master_pct = 0.0;
        if (master_pct > 100.0) master_pct = 100.0;

        double line1_sp_bpm = LINE1_RATED_BPM * (master_pct / 100.0);
        double line2_sp_bpm = LINE2_RATED_BPM * (master_pct / 100.0);
        Supervisor.master_speed_sp = float(master_pct);
        Supervisor.line1.speed_sp = float(line1_sp_bpm);
        Supervisor.line2.speed_sp = float(line2_sp_bpm);
        Supervisor.line1.rated = float(LINE1_RATED_BPM);
        Supervisor.line2.rated = float(LINE2_RATED_BPM);

        // ── 4. Shared product supply / CIP skid ─────────────────────────────
        double consumption_lpm =
            (double(Line1Filler.carousel.speed_fb) / 100.0 * line1_sp_bpm * LINE1_FILL_TARGET_ML / 1000.0) +
                (double(Line2Filler.carousel.speed_fb) / 100.0 * line2_sp_bpm * LINE2_FILL_TARGET_ML / 1000.0);

        bool cip_running = ProductSupply.cip.active;
        double makeup_lpm = (!cip_running && tank_l / TANK_CAPACITY_L < 0.85) ? 400.0 : 0.0;
        tank_l += (makeup_lpm - (cip_running ? 0.0 : consumption_lpm)) * DT / 60.0;
        if (tank_l < 0.0) tank_l = 0.0;
        if (tank_l > TANK_CAPACITY_L) tank_l = TANK_CAPACITY_L;

        ProductSupply.tank.capacity = float(TANK_CAPACITY_L);
        ProductSupply.tank.volume = float(tank_l);
        ProductSupply.tank.level = float(tank_l / TANK_CAPACITY_L * 100.0);
        ProductSupply.tank.low_level_alarm = (tank_l / TANK_CAPACITY_L * 100.0) < 15.0;
        ProductSupply.tank.temp = 4.0f; // chilled product, held near constant
        ProductSupply.tank.agitator.running = !cip_running;
        driveMotor(ProductSupply.tank.agitator, ProductSupply.tank.agitator.running ? 40.0 : 0.0, DT);
        driveMotor(ProductSupply.supply_pump, cip_running ? 0.0 : 70.0, DT);

        ProductSupply.line1_supply.valve.command = (plant_running && !cip_running) ? 100.0f : 0.0f;
        ProductSupply.line2_supply.valve.command = (plant_running && !cip_running) ? 100.0f : 0.0f;
        driveValve(ProductSupply.line1_supply.valve, DT);
        driveValve(ProductSupply.line2_supply.valve, DT);
        ProductSupply.line1_supply.press = float(double(ProductSupply.line1_supply.valve.position) / 100.0 * 2.4);
        ProductSupply.line2_supply.press = float(double(ProductSupply.line2_supply.valve.position) / 100.0 * 2.4);

        // CIP sequencer (only runs when the OPC-UA client sets cip.active and
        // plant is not running product)
        if (cip_running) {
            ProductSupply.cip.time_in_state += float(DT);
            float dwell = ProductSupply.cip.time_in_state;
            uint8 st = ProductSupply.cip.cycle_state;
            const float STAGE_S = 30.0f; // shortened for simulation purposes
            if (st == 0) { ProductSupply.cip.cycle_state = 1; ProductSupply.cip.time_in_state = 0.0f; } else if (dwell > STAGE_S && st < 6) {
                ProductSupply.cip.cycle_state = st + 1;
                ProductSupply.cip.time_in_state = 0.0f;
                if (st + 1 == 6) {
                    ProductSupply.cip.cycles_completed = ProductSupply.cip.cycles_completed + 1;
                    ProductSupply.cip.active = false;
                }
            }
            ProductSupply.cip.supply_temp = (st == 2) ? 75.0f : (st == 4) ? 60.0f : 20.0f;
            ProductSupply.cip.caustic_conc = (st == 2) ? 2.0f : 0.0f;
            ProductSupply.cip.supply_flow = 250.0f;
            ProductSupply.cip.return_conductivity = (st == 2) ? 45.0f : (st >= 3) ? 5.0f : 1.0f;
        } else {
            ProductSupply.cip.cycle_state = 0;
        }

        // ── 5. Line 1 chain ──────────────────────────────────────────────────
        double l1_avail = plant_running ? 1.0 : 0.0;

        // Infeed
        Line1Infeed.infeed_starved = false; // depalletizer assumed well-stocked
        double l1_infeed_bpm = line1_sp_bpm * l1_avail;
        driveMotor(Line1Infeed.unscrambler, l1_infeed_bpm / LINE1_RATED_BPM * 100.0, DT);
        Line1Infeed.bottles_staged = uint16(double(Line1Infeed.table_fill) / 100.0 * 400.0);
        Line1Infeed.table_fill = float(70.0); // fed by depalletizer, assumed regulated upstream

        // Rinser (paces to filler bowl buffer)
        double l1_rinse_trim = trimFeeding(double(ConveyorNetwork.line1.rinser_to_filler.fill));
        double l1_rinser_bpm = l1_infeed_bpm * l1_rinse_trim;
        driveMotor(Line1Rinser.rinser_turret, l1_rinser_bpm / LINE1_RATED_BPM * 100.0, DT);
        Line1Rinser.water_valve.command = plant_running ? 100.0f : 0.0f;
        Line1Rinser.air_valve.command = plant_running ? 100.0f : 0.0f;
        driveValve(Line1Rinser.water_valve, DT);
        driveValve(Line1Rinser.air_valve, DT);
        Line1Rinser.rinse_water_press = float(double(Line1Rinser.water_valve.position) / 100.0 * 3.0);
        Line1Rinser.rinse_air_press = float(double(Line1Rinser.air_valve.position) / 100.0 * 5.5);
        Line1Rinser.nozzles_total = 48;

        updateZone(ConveyorNetwork.line1.rinser_to_filler, l1_rinser_bpm, 0.0, DT, 60.0);

        // Filler (the pacing/bottleneck station — draws from rinser buffer,
        // trimmed by both the rinser buffer starvation and the downstream
        // capper buffer backing up)
        double l1_filler_draw_trim = trimDrawing(double(ConveyorNetwork.line1.rinser_to_filler.fill));
        double l1_filler_push_trim = trimFeeding(double(ConveyorNetwork.line1.filler_to_capper.fill));
        double l1_filler_bpm = line1_sp_bpm * l1_filler_draw_trim * l1_filler_push_trim;
        driveMotor(Line1Filler.carousel, l1_filler_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line1.rinser_to_filler, 0.0, l1_filler_bpm, DT, 60.0); // consume side

        Line1Filler.valve_count = 40;
        Line1Filler.valves_open_now = uint16(l1_filler_bpm / LINE1_RATED_BPM * 40.0);
        Line1Filler.fill_volume.sp = float(LINE1_FILL_TARGET_ML);
        Line1Filler.bowl_level.setpoint = 65.0;
        Line1Filler.bowl_level.process_value = 65.0 + (ProductSupply.tank.level < 20.0 ? -8.0 : 0.0);
        Line1Filler.bowl_level.enabled = plant_running;
        double l1_bowl_err = double(Line1Filler.bowl_level.setpoint) - double(Line1Filler.bowl_level.process_value);
        Line1Filler.bowl_level.output = float(50.0 + l1_bowl_err * 3.0);
        Line1Filler.product_temp = 4.2f;
        Line1Filler.co2_volumes = 0.0f; // still product
        double l1_fill_noise = (double(iteration % 7) - 3.0) * 0.15; // deterministic pseudo-variance
        Line1Filler.fill_volume.avg = float(LINE1_FILL_TARGET_ML + l1_fill_noise);
        Line1Filler.fill_volume.stddev = 0.9f;
        Line1Filler.vacuum_snift_press = -0.35f;

        updateZone(ConveyorNetwork.line1.filler_to_capper, l1_filler_bpm, 0.0, DT, 80.0);

        // Capper
        double l1_capper_draw_trim = trimDrawing(double(ConveyorNetwork.line1.filler_to_capper.fill));
        double l1_capper_push_trim = trimFeeding(double(ConveyorNetwork.line1.capper_to_labeler.fill));
        double l1_capper_bpm = line1_sp_bpm * l1_capper_draw_trim * l1_capper_push_trim;
        driveMotor(Line1Capper.capper_turret, l1_capper_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line1.filler_to_capper, 0.0, l1_capper_bpm, DT, 80.0);

        Line1Capper.chuck_heads = 12;
        Line1Capper.cap_feeder_bowl_level = 55.0f;
        Line1Capper.cap_feeder_starved = false;
        Line1Capper.torque.sp = float(LINE1_TORQUE_TARGET_NM);
        Line1Capper.torque.avg = float(LINE1_TORQUE_TARGET_NM + (double(iteration % 5) - 2.0) * 0.05);
        Line1Capper.torque.stddev = 0.12f;

        updateZone(ConveyorNetwork.line1.capper_to_labeler, l1_capper_bpm, 0.0, DT, 80.0);

        // Labeler
        double l1_label_draw_trim = trimDrawing(double(ConveyorNetwork.line1.capper_to_labeler.fill));
        double l1_label_push_trim = trimFeeding(double(ConveyorNetwork.line1.labeler_to_packer.fill));
        double l1_labeler_bpm = line1_sp_bpm * l1_label_draw_trim * l1_label_push_trim;
        driveMotor(Line1Labeler.labeler_turret, l1_labeler_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line1.capper_to_labeler, 0.0, l1_labeler_bpm, DT, 80.0);

        double l1_label_use = l1_labeler_bpm * DT / 60.0 * 0.0008; // % roll consumed per bottle, scaled
        Line1Labeler.label_supply.front = float(fmax(0.0, double(Line1Labeler.label_supply.front) - l1_label_use));
        Line1Labeler.label_supply.back = float(fmax(0.0, double(Line1Labeler.label_supply.back) - l1_label_use));
        Line1Labeler.applicator_pressure = 2.1f;

        updateZone(ConveyorNetwork.line1.labeler_to_packer, l1_labeler_bpm, 0.0, DT, 100.0);

        // Case packer
        double l1_pack_draw_trim = trimDrawing(double(ConveyorNetwork.line1.labeler_to_packer.fill));
        double l1_packer_bpm = line1_sp_bpm * l1_pack_draw_trim;
        driveMotor(Line1CasePacker.erector, l1_packer_bpm / LINE1_RATED_BPM * 100.0, DT);
        driveMotor(Line1CasePacker.packer, l1_packer_bpm / LINE1_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line1.labeler_to_packer, 0.0, l1_packer_bpm, DT, 100.0);

        Line1CasePacker.bottles_per_case = 12;
        Line1CasePacker.glue.tank_temp.setpoint = 165.0;
        Line1CasePacker.glue.tank_temp.process_value = 165.0 - (plant_running ? 0.0 : 20.0);
        Line1CasePacker.glue.tank_temp.enabled = true;
        Line1CasePacker.glue.pressure = 4.5f;
        Line1CasePacker.case_blank.hopper = 60.0f;
        double l1_cases_this_scan = l1_packer_bpm * DT / 60.0 / 12.0;
        Line1CasePacker.cases_packed_count += uint32(l1_cases_this_scan);
        Supervisor.line1.good_count += uint32(l1_packer_bpm * DT / 60.0);
        Supervisor.line1.actual = float(l1_packer_bpm);
        Supervisor.line1.oee = float(line1_sp_bpm > 0.0 ? (l1_packer_bpm / line1_sp_bpm * 100.0) : 0.0);

        // ── 6. Line 2 chain (simplified — no rinser, PET/can line) ──────────
        double l2_avail = plant_running ? 1.0 : 0.0;
        double l2_infeed_bpm = line2_sp_bpm * l2_avail;
        driveMotor(Line2Infeed.unscrambler, l2_infeed_bpm / LINE2_RATED_BPM * 100.0, DT);
        Line2Infeed.infeed_starved = false;
        Line2Infeed.table_fill = 70.0f;

        double l2_filler_draw_trim = 1.0; // infeed assumed non-limiting for Line 2
        double l2_filler_push_trim = trimFeeding(double(ConveyorNetwork.line2.filler_to_capper.fill));
        double l2_filler_bpm = line2_sp_bpm * l2_filler_draw_trim * l2_filler_push_trim;
        driveMotor(Line2Filler.carousel, l2_filler_bpm / LINE2_RATED_BPM * 100.0, DT);

        Line2Filler.valve_count = 24;
        Line2Filler.valves_open_now = uint16(l2_filler_bpm / LINE2_RATED_BPM * 24.0);
        Line2Filler.fill_volume.sp = float(LINE2_FILL_TARGET_ML);
        Line2Filler.bowl_level.setpoint = 65.0;
        Line2Filler.bowl_level.process_value = 65.0 + (ProductSupply.tank.level < 20.0 ? -8.0 : 0.0);
        Line2Filler.bowl_level.enabled = plant_running;
        Line2Filler.product_temp = 4.2f;
        Line2Filler.co2_volumes = 3.8f; // carbonated
        double l2_fill_noise = (double(iteration % 9) - 4.0) * 0.1;
        Line2Filler.fill_volume.avg = float(LINE2_FILL_TARGET_ML + l2_fill_noise);
        Line2Filler.fill_volume.stddev = 0.6f;
        Line2Filler.vacuum_snift_press = -0.3f;

        updateZone(ConveyorNetwork.line2.filler_to_capper, l2_filler_bpm, 0.0, DT, 100.0);

        double l2_capper_draw_trim = trimDrawing(double(ConveyorNetwork.line2.filler_to_capper.fill));
        double l2_capper_push_trim = trimFeeding(double(ConveyorNetwork.line2.capper_to_labeler.fill));
        double l2_capper_bpm = line2_sp_bpm * l2_capper_draw_trim * l2_capper_push_trim;
        driveMotor(Line2_Capper.capper_turret, l2_capper_bpm / LINE2_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line2.filler_to_capper, 0.0, l2_capper_bpm, DT, 100.0);

        Line2_Capper.chuck_heads = 8;
        Line2_Capper.torque.sp = float(LINE2_TORQUE_TARGET_NM);
        Line2_Capper.torque.avg = float(LINE2_TORQUE_TARGET_NM + (double(iteration % 5) - 2.0) * 0.03);
        Line2_Capper.torque.stddev = 0.08f;

        updateZone(ConveyorNetwork.line2.capper_to_labeler, l2_capper_bpm, 0.0, DT, 100.0);

        double l2_label_draw_trim = trimDrawing(double(ConveyorNetwork.line2.capper_to_labeler.fill));
        double l2_label_push_trim = trimFeeding(double(ConveyorNetwork.line2.labeler_to_packer.fill));
        double l2_labeler_bpm = line2_sp_bpm * l2_label_draw_trim * l2_label_push_trim;
        driveMotor(Line2Labeler.labeler_turret, l2_labeler_bpm / LINE2_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line2.capper_to_labeler, 0.0, l2_labeler_bpm, DT, 100.0);
        Line2Labeler.label_supply.front = float(fmax(0.0, double(Line2Labeler.label_supply.front) - l2_labeler_bpm * DT / 60.0 * 0.0006));
        Line2Labeler.label_supply.back = Line2Labeler.label_supply.front;
        Line2Labeler.applicator_pressure = 1.8f;

        updateZone(ConveyorNetwork.line2.labeler_to_packer, l2_labeler_bpm, 0.0, DT, 120.0);

        double l2_pack_draw_trim = trimDrawing(double(ConveyorNetwork.line2.labeler_to_packer.fill));
        double l2_packer_bpm = line2_sp_bpm * l2_pack_draw_trim;
        driveMotor(Line2CasePacker.erector, l2_packer_bpm / LINE2_RATED_BPM * 100.0, DT);
        driveMotor(Line2CasePacker.packer, l2_packer_bpm / LINE2_RATED_BPM * 100.0, DT);
        updateZone(ConveyorNetwork.line2.labeler_to_packer, 0.0, l2_packer_bpm, DT, 120.0);

        Line2CasePacker.bottles_per_case = 24;
        Line2CasePacker.glue.tank_temp.setpoint = 165.0;
        Line2CasePacker.glue.tank_temp.process_value = 165.0 - (plant_running ? 0.0 : 20.0);
        Line2CasePacker.glue.pressure = 4.3f;
        Line2CasePacker.case_blank.hopper = 60.0f;
        double l2_cases_this_scan = l2_packer_bpm * DT / 60.0 / 24.0;
        Line2CasePacker.cases_packed_count += uint32(l2_cases_this_scan);
        Supervisor.line2.good_count += uint32(l2_packer_bpm * DT / 60.0);
        Supervisor.line2.actual = float(l2_packer_bpm);
        Supervisor.line2.oee = float(line2_sp_bpm > 0.0 ? (l2_packer_bpm / line2_sp_bpm * 100.0) : 0.0);

        // ── 7. Merge conveyor + shared palletizer ────────────────────────────
        double merge_in_cases_min = l1_cases_this_scan / (DT / 60.0) + l2_cases_this_scan / (DT / 60.0);
        driveMotor(ConveyorNetwork.merge.conveyor, merge_in_cases_min > 0.1 ? 80.0 : 0.0, DT);
        ConveyorNetwork.merge.line1_gate.command = 100.0f;
        ConveyorNetwork.merge.line2_gate.command = 100.0f;
        driveValve(ConveyorNetwork.merge.line1_gate, DT);
        driveValve(ConveyorNetwork.merge.line2_gate, DT);
        updateZone(ConveyorNetwork.merge.zone, merge_in_cases_min, 0.0, DT, 40.0);

        double palletizer_trim = trimDrawing(double(ConveyorNetwork.merge.zone.fill));
        double palletizer_cases_min = merge_in_cases_min * palletizer_trim;
        updateZone(ConveyorNetwork.merge.zone, 0.0, palletizer_cases_min, DT, 40.0);

        bool robot_active = plant_running && palletizer_cases_min > 0.05;
        Palletizer.robot.running = robot_active;
        Palletizer.cases_per_layer = 10;
        Palletizer.layers_per_pallet = 6;
        double cases_this_scan = palletizer_cases_min * DT / 60.0;
        int total_case_slot = int(double(Palletizer.current_pallet_case_count) + cases_this_scan);
        Palletizer.current_pallet_case_count = uint16(total_case_slot % 60);
        Palletizer.current_layer = uint16((total_case_slot % 60) / 10);
        Palletizer.pallet_height = float(150.0 + double(Palletizer.current_layer) * 220.0);
        Palletizer.robot.cycle_state = robot_active ? uint8(1) : uint8(0);

        bool pallet_complete = total_case_slot >= 60;
        if (pallet_complete) {
            Palletizer.pallets_completed_count = Palletizer.pallets_completed_count + 1;
            Palletizer.wrapper.active = true;
            Palletizer.wrapper.turntable_speed = 8.0f;
            Palletizer.wrapper.film_tension = 35.0f;
            Palletizer.wrapper.wraps_target = 4;
            Palletizer.wrapper.wraps_completed = 4;
        } else {
            Palletizer.wrapper.active = false;
            Palletizer.wrapper.turntable_speed = 0.0f;
        }
        driveMotor(Palletizer.full_pallet_conveyor, pallet_complete ? 50.0 : 0.0, DT);
        Palletizer.empty_pallet_dispenser_starved = false;

        // ── 8. Plant utilities ───────────────────────────────────────────────
        double air_demand = plant_running ? 1.0 : 0.3;
        air_press += ((AIR_HEADER_SP_BAR - air_press) * 0.3 - air_demand * 0.15) * DT;
        if (air_press < 0.0) air_press = 0.0;
        Utilities.air.header_press_sp = float(AIR_HEADER_SP_BAR);
        Utilities.air.header_press = float(air_press);
        Utilities.air.low_press_alarm = air_press < 5.5;
        driveMotor(Utilities.air.compressor_1, 70.0, DT);
        driveMotor(Utilities.air.compressor_2, air_press < 6.5 ? 70.0 : 0.0, DT);
        Utilities.air.dryer_dewpoint = -40.0f;

        glycol_supply_c += ((GLYCOL_SP_C - glycol_supply_c) * 0.2) * DT;
        Utilities.glycol.supply_temp.setpoint = GLYCOL_SP_C;
        Utilities.glycol.supply_temp.process_value = glycol_supply_c;
        Utilities.glycol.supply_temp.enabled = true;
        Utilities.glycol.return_temp = float(glycol_supply_c + 3.5);
        driveMotor(Utilities.glycol.chiller_compressor, 65.0, DT);
        driveMotor(Utilities.glycol.pump, 60.0, DT);

        Utilities.vacuum.header = -68.0f;
        driveMotor(Utilities.vacuum.pump, plant_running ? 55.0 : 0.0, DT);

        // ── 9. Alarm aggregation — critical, pushed immediately ─────────────
        uint alarm_count = 0;
        if (ProductSupply.tank.low_level_alarm) alarm_count++;
        if (Utilities.air.low_press_alarm) alarm_count++;
        if (SafetySystems.any_estop_active) alarm_count++;
        if (Line1Infeed.jam_detected) alarm_count++;
        if (Line2Infeed.jam_detected) alarm_count++;

        Alarms.active_count = uint16(alarm_count);
        Alarms.any_active = alarm_count > 0;
        Alarms.plant_estop_active = plant_estop;
        Alarms.line1_fault_active = Supervisor.line1.mode == 4;
        Alarms.line2_fault_active = Supervisor.line2.mode == 4;
        Alarms.utilities_fault_active = Utilities.air.low_press_alarm;
        Alarms.any_critical = plant_estop;
        Alarms.critical_count = plant_estop ? 1 : 0;
        Alarms.timestamp = ts;
        Alarms.put(); // flush now — don't wait behind the routine telemetry below

        // ── 10. Publish routine telemetry ────────────────────────────────────
        // SafetySystems, Supervisor and Alarms were already flushed above
        // (steps 2 and 9) the moment their critical fields were computed.
        // Everything else is routine process telemetry — dirty tracking
        // means these calls are cheap even though they run every scan; only
        // fields that actually moved since the last .put() go out on the wire.
        Supervisor.timestamp = ts;
        Supervisor.put(); // flushes speed_sp/oee/counts set since step 2
        ProductSupply.timestamp = ts;
        ProductSupply.put();
        Line1Infeed.timestamp = ts;
        Line1Infeed.put();
        Line1Rinser.timestamp = ts;
        Line1Rinser.put();
        Line1Filler.timestamp = ts;
        Line1Filler.put();
        Line1Capper.timestamp = ts;
        Line1Capper.put();
        Line1Labeler.timestamp = ts;
        Line1Labeler.put();
        Line1CasePacker.timestamp = ts;
        Line1CasePacker.put();
        Line2Infeed.timestamp = ts;
        Line2Infeed.put();
        Line2Filler.timestamp = ts;
        Line2Filler.put();
        Line2_Capper.timestamp = ts;
        Line2_Capper.put();
        Line2Labeler.timestamp = ts;
        Line2Labeler.put();
        Line2CasePacker.timestamp = ts;
        Line2CasePacker.put();
        ConveyorNetwork.timestamp = ts;
        ConveyorNetwork.put();
        Palletizer.timestamp = ts;
        Palletizer.put();
        Utilities.timestamp = ts;
        Utilities.put();

        iteration++;
        sleep(int(1000.0 / SCAN_HZ));
    }
}
