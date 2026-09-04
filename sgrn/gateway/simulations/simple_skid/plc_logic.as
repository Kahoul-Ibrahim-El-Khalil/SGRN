// plc_logic.as — Simple Tank & Pump Skid Logic

float level = 30.0f;
float temp = 22.0f;
bool pump_on = true;
bool heater_on = false;

void main() {
    print("[simple_skid] Starting Tank Skid simulation loop...");
    
    while (true) {
        if (pump_on) {
            level += 0.5f;
            if (level >= 90.0f) {
                pump_on = false;
                heater_on = true;
            }
        } else {
            level -= 0.8f;
            if (level <= 20.0f) {
                pump_on = true;
                heater_on = false;
            }
        }
        
        if (heater_on) {
            temp += 0.3f;
        } else if (temp > 22.0f) {
            temp -= 0.1f;
        }

        TankSkid.tank_level = level;
        TankSkid.fluid_temp = temp;
        TankSkid.pump_running = pump_on;
        TankSkid.heater_on = heater_on;
        
        sleep_ms(100);
    }
}
