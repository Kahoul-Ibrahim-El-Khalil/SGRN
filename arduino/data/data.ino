#include <WiFi.h>
#include <Settimino.h>
#include <time.h>
#include "../../extern/s7codec/s7.hpp"

// WiFi Configuration
const char* ssid     = "KIX";
const char* password = "Beethoven";

// NTP Configuration
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 3600;
const int   daylightOffset_sec = 0;

// S7 Node Configuration
IPAddress S7NodeIP(10, 19, 98, 27);

// S7 Client Instance
S7Client Client;

// ─────────────────────────────────────────────────────────────────────────────
// DB2 — SensorData  (must match the PLC Data Block layout exactly)
//
// Every field is a s7codec:: type so the packed struct is already in S7
// wire-format; no manual byte-swapping is needed anywhere.
// ─────────────────────────────────────────────────────────────────────────────

DATABLOCK(S7SensorDB) {
    // Offset  0 — REAL   temperature  (°C)
    s7codec::Real    temperature;

    // Offset  4 — REAL   pressure     (hPa)
    s7codec::Real    pressure;

    // Offset  8 — REAL   flow_rate    (L/min)
    s7codec::Real    flow_rate;

    // Offset 12 — REAL   level        (%)
    s7codec::Real    level;

    // Offset 16 — INT    status_code
    s7codec::Int     status_code;

    // Offset 18 — WORD   alarm_bits
    s7codec::Word    alarm_bits;

    // Offset 20 — STRING[16]  unit_id  (matches STRING[16] in TIA Portal)
    s7codec::String<16> unit_id;

    // Offset 38 — DATE   calibration_date  (days since 1990-01-01)
    s7codec::Date    calibration_date;

    // Offset 40 — TOD    last_poll_time    (ms since midnight)
    s7codec::TOD     last_poll_time;

    // Offset 44 — DTL    timestamp         (full date + time + ns)
    s7codec::DTL       timestamp;
};

static_assert(sizeof(S7SensorDB) == 56,
              "S7SensorDB layout does not match the PLC DB definition");

// Global application state
S7SensorDB current_data;
S7SensorDB last_data;

// ─────────────────────────────────────────────────────────────────────────────
// Business Logic
// ─────────────────────────────────────────────────────────────────────────────

/// Populate every time-related field from the current NTP-synchronised clock.
/// Returns false if the system clock is not yet ready.
bool updateTimeFields(S7SensorDB& db) {
    struct tm ti;
    if (!getLocalTime(&ti)) return false;

    // DTL, S7TOD and Date all accept a struct tm directly.
    db.timestamp        = ti;            // DTL::operator=(struct tm)
    db.last_poll_time   = ti;            // S7TOD::operator=(struct tm)
    db.calibration_date = ti;            // Date::operator=(struct tm)
    return true;
}

void connectWifi() {
    Serial.printf("Connecting to %s ", ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\n[SYSTEM] WiFi Connected. Synchronizing NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void initStaticFields() {
    // String<16>  — assignment from string literal; handles length clamping
    current_data.unit_id     = "ESP32-SENSOR-01";
    current_data.status_code = 0;
    current_data.alarm_bits  = 0u;
}

void randomizeSensors() {
    // Direct assignment — Real (= BigEndian<float>) stores big-endian bytes.
    current_data.temperature = 20.0f   + (random(0, 100) / 10.0f);
    current_data.pressure    = 1013.0f + (random(0, 500) / 10.0f);
    current_data.flow_rate   = 50.0f   + (random(0, 200) / 10.0f);
    current_data.level       = 75.0f   + (random(0, 100) / 10.0f);

    if (!updateTimeFields(current_data)) {
        Serial.println("[WARN] NTP time not available; time fields unchanged.");
    }
}

void onChangeSend() {
    randomizeSensors();

    // Compare only the four float fields (first 16 bytes) for change detection.
    // offsetof is used so the comparison range stays correct if fields move.
    if (memcmp(&current_data, &last_data, offsetof(S7SensorDB, status_code)) != 0) {
        if (!Client.Connected) {
            Serial.println("[SYSTEM] Connecting to S7 Central Node...");
            if (Client.ConnectTo(S7NodeIP, 0, 0) != 0) {
                Serial.println("[ERROR] S7 Connection Failed");
                return;
            }
        }

        int res = Client.WriteArea(S7AreaDB, 2, 0, sizeof(S7SensorDB), &current_data);

        if (res == 0) {
            Serial.println("[SUCCESS] SensorData (DB2) flushed to Central Node.");
            // Read back as native C++ types for the serial log — no manual swapping.
            Serial.printf("  temp=%.1f  pres=%.1f  flow=%.1f  lvl=%.1f  unit=%.*s\n",
                (float)current_data.temperature,
                (float)current_data.pressure,
                (float)current_data.flow_rate,
                (float)current_data.level,
                (int)current_data.unit_id.size(),
                current_data.unit_id.view().data());
            memcpy(&last_data, &current_data, sizeof(S7SensorDB));
        } else {
            Serial.printf("[ERROR] S7 Write Failed: 0x%04X\n", res);
            if (res & 0x00FF) Client.Disconnect();
        }
    }
}

void setup() {
    Serial.begin(115200);
    connectWifi();
    randomSeed(analogRead(0));
    memset(&last_data, 0, sizeof(S7SensorDB));
    initStaticFields();
}

void loop() {
    onChangeSend();
    delay(500);
}
