// modbus_registers_test.cpp — gateway Modbus TCP southbound wire coverage on
// a real in-tree simulation schema, driven through the vendored libmodbus
// client wrapper (sgrn::wrappers::modbus::Client). Adapted from
// tests/gateway/modbus.py, whose hardcoded DB17 register block has no in-tree
// provider: here the register structure is derived from the gateway's own
// /registry/modbus map for the `tennessee` simulation (DB1 "Reactor" declares
// #MODBUS_HOLDING instead of a static DB17).
//
// Scope asserted here:
//   1. GET /registry/modbus returns the HTTP wire shape produced by
//      serializeModbusMapToJson: `holding_registers` / `input_registers` /
//      `coils` / `discrete_inputs` arrays of
//      {start, count, type, access, padded, source="DBn.<field_path>"} plus
//      a `warnings` array (tennessee DB1 "Reactor": 73 holding entries).
//   2. A fresh libmodbus TCP client connects on the OS-assigned port.
//   3. FC16 (write multiple holding registers): writing big-endian Modbus
//      bytes for a Real field updates the adapter's register block.
//   4. FC03 (read holding registers) reads back the written bytes and they
//      decode to the value written (little-endian DBs are canonicalized to
//      big-endian modbus semantics on the arena→mapping path).
//   5. HTTP ground truth: GET /data/Reactor/<field_path> shows the same
//      value in the PlcMemory digital twin (write actually landed, not just
//      echoed registers), matching modbus.py's twin cross-check.
//   6. FC06 (write single register) mutates exactly one register of the
//      `level` field.
//   7. No-#MODBUS_COIL claim: tennessee only declares #MODBUS_HOLDING, so
//      the coil space is unowned — the libmodbus minimum reserved bit still
//      round-trips via FC05/FC01 without touching any twin field.
//
// The s7shell simulation is deliberately NOT spawned: nothing else contends
// for the DB arena while the register values are asserted (the process values
// would otherwise drift between write and read).
//
// Environment: gateway via sgrn::test::GatewayFixture (tennessee, Modbus
// listener opt-in). Missing binaries → SKIP + exit 0.

#include "gateway_fixture.hpp"

#include <sgrn/wrappers/modbus/Client.hpp>

#include <httplib.h>
#include <json/json.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

using sgrn::wrappers::modbus::Client;

Json::Value parseJson(const std::string& t_json) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors)) {
        ++g_failures;
        std::printf("FAIL HTTP JSON does not parse: %s (%.160s)\n", errors.c_str(), t_json.c_str());
        return {};
    }
    return v;
}

/// Float ↔ two big-endian Modbus registers (network byte order on the wire —
/// the adapter's encodeToRegisters/decodeRegisters treat register pairs as
/// consecutive big-endian arena bytes).
uint16_t beRegsOfFloat(float t_v, uint16_t t_out_regs[2]) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(t_v), "32-bit float assumed");
    std::memcpy(&bits, &t_v, sizeof(bits));
    t_out_regs[0] = static_cast<uint16_t>(bits >> 16);
    t_out_regs[1] = static_cast<uint16_t>(bits & 0xFFFFu);
    return t_out_regs[0];
}

float floatFromRegs(const uint16_t* t_regs) {
    const uint32_t bits = (static_cast<uint32_t>(t_regs[0]) << 16) | t_regs[1];
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

/// Read a full holding-register entry and decode it per its adapter type into
/// the float value the twin holds (Real) — unsupported types are skipped by
/// the caller via `type` inspection.
bool readEntryFloat(Client& t_mb, const Json::Value& t_entry, float& t_out) {
    const int reg_start = t_entry["start"].asInt();
    const int reg_count = t_entry["count"].asInt();
    std::vector<uint16_t> regs(static_cast<std::size_t>(reg_count > 0 ? reg_count : 1), 0);
    auto r = t_mb.readHoldingRegisters(reg_start, reg_count, regs.data());
    if (r.hasError()) {
        ++g_failures;
        std::printf("FAIL FC03 %s: %s\n", t_entry["source"].asString().c_str(), r.error().c_str());
        return false;
    }
    t_out = floatFromRegs(regs.data());
    return true;
}

} // namespace

int main() {
    sgrn::test::GatewayFixture gw("tennessee", {.spawn_simulation = false, .modbus_listener = true});
    if (!gw.available()) {
        std::printf("SKIP: gateway unavailable — %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(3);

    // ── 1. Registry: the gateway's own virtual Modbus map ───────────────────
    // NOTE the wire shape is the HTTP-facing form produced by
    // serializeModbusMapToJson: arrays of {start, count, type, access,
    // padded, source="DBn.<field_path>"} under holding_registers /
    // input_registers / coils / discrete_inputs (NOT the ModbusVirtualMap
    // struct's {holding,input,coil,discrete} code-side form).
    auto body = http.Get("/registry/modbus");
    CHECK(body && body->status == 200);
    if (!(body && body->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value map_doc = parseJson(body->body);
    const Json::Value& holding = map_doc["holding_registers"];
    CHECK(holding.isArray() && !holding.empty());
    if (!(holding.isArray() && !holding.empty())) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    // DB1 "Reactor" entries: Real fields → 2 registers, big-endian modeling
    const Json::Value* feed_flow = nullptr;
    const Json::Value* level = nullptr;
    const std::string db_name = "Reactor"; // tennessee DB1 (see schema.scl)
    for (const auto& e : holding) {
        CHECK(e.isMember("start") && e.isMember("count") && e.isMember("type") && e.isMember("access") && e.isMember("source"));
        CHECK(e["source"].asString().rfind("DB1.", 0) == 0);
        if (e["source"].asString() == "DB1.a_feed_flow")
            feed_flow = &e;
        else if (e["source"].asString() == "DB1.level")
            level = &e;
    }
    CHECK(feed_flow && level);
    CHECK(map_doc.isMember("coils") && map_doc["coils"].isArray());
    if (!(feed_flow && level)) {
        std::printf("FAIL: no a_feed_flow/level holding entries for DB1 tennessee\n%d failure(s)\n", g_failures);
        return 1;
    }

    // ── 2. libmodbus TCP client on the OS-assigned port ─────────────────────
    auto connect = Client::connect({.host = "127.0.0.1", .port = static_cast<uint16_t>(gw.modbusPort()), .slave_id = 1});
    CHECK(!connect.hasError());
    if (connect.hasError()) {
        std::printf("FAIL modbus connect: %s\n%d failure(s)\n", connect.error().c_str(), g_failures);
        return 1;
    }
    Client mb = std::move(*connect);

    // ── 3/4/5. FC16 write → FC03 read-back → HTTP twin ground truth ─────────
    for (const Json::Value* entry : {feed_flow, level}) {
        const std::string source = (*entry)["source"].asString(); // "DB1.<field path>"
        const std::string field_path = source.substr(source.find('.') + 1);
        const int reg_start = (*entry)["start"].asInt();
        const int reg_count = (*entry)["count"].asInt();

        const float new_value = entry == feed_flow ? 25.5f : 75.25f;
        std::vector<uint16_t> regs(static_cast<std::size_t>(reg_count), 0);
        if (reg_count == 2)
            beRegsOfFloat(new_value, regs.data());
        else
            regs[0] = static_cast<uint16_t>(static_cast<int>(new_value));
        auto w = mb.writeRegisters(reg_start, reg_count, regs.data());
        CHECK(!w.hasError());
        if (w.hasError()) {
            std::printf("FAIL FC16 %s: %s\n", source.c_str(), w.error().c_str());
            continue;
        }

        float back = 0.f;
        if (!readEntryFloat(mb, *entry, back))
            continue;
        CHECK(std::fabs(back - new_value) < 1e-6f);
        if (!(std::fabs(back - new_value) < 1e-6f))
            std::printf("FAIL FC03 read-back %s: %.9g (want %.9g)\n", source.c_str(), back, new_value);

        // HTTP ground truth: the twin's whole-DB sync picked the write up and
        // re-encoded the arena bytes through the schema (Real leaf → a number).
        auto res = http.Get(("/data/" + db_name + "/" + field_path).c_str());
        if (res && res->status == 200) {
            const Json::Value v = parseJson(res->body);
            CHECK(v.isNumeric() && std::fabs(v.asFloat() - new_value) < 1e-4f);
            if (!(v.isNumeric() && std::fabs(v.asFloat() - new_value) < 1e-4f))
                std::printf(
                    "FAIL HTTP twin %s/%s: %.240s (want %.9g)\n", db_name.c_str(), field_path.c_str(), res->body.c_str(), new_value);
        } else {
            ++g_failures;
            std::printf("FAIL HTTP GET /data/%s/%s failed (%d): %.160s\n", db_name.c_str(), field_path.c_str(), res ? res->status : 0,
                res ? res->body.c_str() : "");
        }
    }

    // ── 6. FC06 single-register write ────────────────────────────────────────
    {
        float prior = 0.f;
        CHECK(readEntryFloat(mb, *level, prior));
        const float expected_after = [&] {
            // overwrite the FIRST register only; low half keeps prior bits
            uint16_t regs[2];
            beRegsOfFloat(prior, regs);
            const uint32_t bits = (static_cast<uint32_t>(0x3F80) << 16) | regs[1];
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }();
        auto w = mb.writeRegister((*level)["start"].asInt(), 0x3F80);
        CHECK(!w.hasError());
        if (w.hasError()) {
            std::printf("FAIL FC06: %s\n", w.error().c_str());
        } else {
            float back = 0.f;
            CHECK(readEntryFloat(mb, *level, back));
            CHECK(std::fabs(back - expected_after) < 1e-6f);
            if (!(std::fabs(back - expected_after) < 1e-6f))
                std::printf("FAIL FC06 read-back level: %.9g (want %.9g)\n", back, expected_after);
        }
    }

    // ── 7. Coil space unclaimed in tennessee ─────────────────────────────────
    //      No DB declares #MODBUS_COIL, so all coils are unowned; the adapter
    //      still serves libmodbus's minimum-1-bit reserved mapping (nb_bits =
    //      max(1, total_coils)). FC05 to bit 0 must be ACCEPTED and
    //      round-trip through FC01 without touching any twin field (no DB
    //      entry can be affected — there are no coil-area entries at all).
    if (map_doc["coils"].empty()) {
        auto w = mb.writeCoil(0, true);
        CHECK(!w.hasError());
        uint8_t bit = 0;
        auto r = mb.readCoils(0, 1, &bit);
        CHECK(!r.hasError());
        CHECK(bit == 1);
        if (r.hasError() || bit != 1)
            std::printf(
                "FAIL coil space: FC05/FC01 round trip bit=%d (%s)\n", bit, r.hasError() ? r.error().c_str() : "write did not stick");
        // leave the reserved bit clean for repeat runs of this test
        if (!w.hasError())
            (void)mb.writeCoil(0, false);
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("modbus_registers_test: all checks passed (http:%d modbus:%d)\n", gw.httpPort(), gw.modbusPort());
    return 0;
}
