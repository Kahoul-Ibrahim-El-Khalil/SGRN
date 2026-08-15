// SGRN S7Gateway | RO System Simulation Script (AngelScript)
// Respects the schema in code.scl

const string IP = "192.168.2.4";
void main() {
    print(">>> SGRN RO System Simulation Started");

    // Connect to S7Gateway (assuming it is configured with DB1 = Data from code.scl)
    S7Client@ plc = S7Client(IP, 0, 1);
    plc.loadSclSchema("code.scl"); 
    if (!plc.isConnected()) {
        print("[ERROR] Could not connect to S7Gateway at" + IP + ":102");
        return;
    }

    print("[INFO] Connected. Starting elaborate Desalination Plant Simulation...");

    // Access the "Data" block (DB2 based on code.scl)
    DataBlock@ data = plc.db("Data");

    // Process Variables
    float feed_level = 65.0f;
    float permeate_level = 10.0f;
    float pressure_hpp = 0.5f;
    float orp = 150.0f;
    float conductivity = 450.0f;
    float ph = 7.2f;
    float flow_after_ro = 0.0f;
    
    // Simulation States: 0=Idle, 1=Intake, 2=RO_Process, 3=Flushing, 4=Emergency
    int state = 1; 
    uint cycle = 0;

    // Use memory facade for low-level writes
    S7Memory@ mem = plc.memory();

    while(true) {
        cycle++;
        
        // --- 1. SENSOR SIMULATION (Physics Engine) ---
        // [Same logic as before...]
        
        // --- 2. STATE MACHINE (PLC LOGIC INJECTION) ---
        // [Same logic as before...]

        // --- 3. DIRECT MEMORY WRITES (Low-level, bypass batch engine) ---
        // Using plc.memory() avoids MultiVar overhead. writeAddress expects (address, hex_string)
        mem.writeAddress("DB2.0", formatDoubleToHex(feed_level)); 
        
        // Use symbolic writes
        data.write("Sensors.Level_feed", int((feed_level / 100.0f) * 27648));
        
        // --- 4. COMMIT PARTIAL PUSH ---
        // This triggers the automatic dirty-region comparison
        data.put(); 

        if (cycle % 10 == 0) {
            print("Cycle: " + cycle + " | Level: " + feed_level + "% | Pressure: " + pressure_hpp + " bar");
        }

        sleep(200); 
    }
}
