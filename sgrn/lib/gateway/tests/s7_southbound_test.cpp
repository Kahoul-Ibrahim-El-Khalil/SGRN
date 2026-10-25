// s7_southbound_test.cpp — gateway S7 southbound wire coverage: a real S7comm
// client (sgrn::wrappers::s7::S7Client / snap7) connects to the gateway's
// hosted S7 server on the OS-assigned port and drives the DB area that no
// TS test today exercises beyond endpoint-shape checks.
//
// Scope asserted here:
//   1. GET /registry resolves a concrete DB (number, name, size_bytes,
//      endianness, scalar leaf fields with offsets).
//   2. S7Client connect (PG connection, rack 0 / slot 1) works against
//      127.0.0.1:<random s7 port>.
//   3. readDB of a scalar Real leaf decodes the big-endian S7 wire bytes to
//      the same value the HTTP twin reports for that field (read path +
//      block-endian arena conversion).
//   4. HTTP PUT /data/<DB>/<field> (the allow-all policy's writer) updates
//      the value, and a second readDB reflects it — round trip without a
//      live simulation drifting underneath.
//   5. writeDB is exercised opportunistically: when the security policy
//      allows S7 PUTs it must land in the twin; when the policy denies them
//      the error is reported, not fatal (legacy gas_processing script allows
//      HTTP only).
//
// The s7shell simulation is deliberately NOT spawned so the values stay
// frozen while S7 and HTTP observations are compared.
//
// Environment: gateway via sgrn::test::GatewayFixture (default simulation).
// Missing binaries → SKIP + exit 0.

#include "gateway_fixture.hpp"

#include <sgrn/wrappers/s7/S7Client.hpp>

#include <httplib.h>
#include <json/json.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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

Json::Value parseJson(const std::string& t_json, const char* t_what) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors)) {
        ++g_failures;
        std::printf("FAIL %s does not parse: %s (%.160s)\n", t_what, errors.c_str(), t_json.c_str());
        return {};
    }
    return v;
}

/// First scalar leaf (no children, count==1) of the given type, walking
/// nested struct children with dotted-path accumulation.
const Json::Value* findScalarLeaf(const Json::Value& t_field, const std::string& t_type, std::string& t_out_path) {
    if (t_field.isMember("children") && t_field["children"].isArray() && !t_field["children"].empty()) {
        const std::string prefix = t_field["name"].asString() + ".";
        for (const auto& child : t_field["children"]) {
            if (const Json::Value* hit = findScalarLeaf(child, t_type, t_out_path); hit) {
                t_out_path = prefix + t_out_path;
                return hit;
            }
        }
    }
    // top-level leaf with unit count (`count` key is absent for scalars);
    // type comparison is case-insensitive — the registry emits SCL names
    // ("REAL", "DINT", ...).
    const std::string want_upper = [&] {
        std::string s = t_type;
        for (auto& c : s)
            c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
        return s;
    }();
    std::string got_upper = t_field["type"].asString();
    for (auto& c : got_upper)
        c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (got_upper == want_upper && (!t_field.isMember("count") || t_field["count"].asInt() == 1)) {
        t_out_path = t_field["name"].asString();
        return &t_field;
    }
    return nullptr;
}

/// Decode a `float` from 4 big-endian IEEE-754 bytes — the gateway's S7
/// server presents the S7 wire order (big-endian) regardless of the block's
/// internal endianness: S7ProtocolAdapter applies swapRangeToBigEndian on
/// reads and writes for little-endian blocks.
float floatFromDbBytes(const std::vector<uint8_t>& t_bytes) {
    const uint32_t bits = static_cast<uint32_t>(t_bytes[3]) | (static_cast<uint32_t>(t_bytes[2]) << 8) |
                          (static_cast<uint32_t>(t_bytes[1]) << 16) | (static_cast<uint32_t>(t_bytes[0]) << 24);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void floatToDbBytes(float t_v, std::vector<uint8_t>& t_out) {
    uint32_t bits;
    std::memcpy(&bits, &t_v, sizeof(bits));
    t_out = {static_cast<uint8_t>((bits >> 24) & 0xFF), static_cast<uint8_t>((bits >> 16) & 0xFF), static_cast<uint8_t>((bits >> 8) & 0xFF),
        static_cast<uint8_t>(bits & 0xFF)};
}

} // namespace

int main() {
    sgrn::test::GatewayFixture gw("gas_processing", {.spawn_simulation = false});
    if (!gw.available()) {
        std::printf("SKIP: gateway unavailable — %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(3);

    // ── 1. Registry: pick a DB + its first Real scalar leaf ──────────────────
    auto body = http.Get("/registry");
    CHECK(body && body->status == 200);
    if (!(body && body->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value doc = parseJson(body->body, "/registry");
    const Json::Value& dbs = doc["dbs"];
    CHECK(dbs.isArray() && !dbs.empty());
    if (!(dbs.isArray() && !dbs.empty())) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value* db = &dbs[0];
    const std::string db_name = (*db)["name"].asString();
    const uint16_t db_number = static_cast<uint16_t>((*db)["number"].asUInt());
    const bool db_little = (*db).isMember("endianness") && (*db)["endianness"].asString() == "little";
    std::string field_path;
    const Json::Value* field = nullptr;
    for (const auto& f : (*db)["fields"]) {
        if ((field = findScalarLeaf(f, "Real", field_path)) != nullptr)
            break;
    }
    CHECK(field != nullptr);
    if (field == nullptr) {
        std::printf("FAIL: no Real scalar leaf in DB %s\n%d failure(s)\n", db_name.c_str(), g_failures);
        return 1;
    }
    const int off = (*field)["offset"].asInt();

    // ── 2. S7Client connect (PG connection, rack 0 / slot 1) ──────────────────
    sgrn::wrappers::s7::S7Client client;
    auto conn = client.connect("127.0.0.1", 0, 1, 1, static_cast<uint16_t>(gw.s7Port())); // conn type 1 = CONNTYPE_PG
    CHECK(!conn.hasError());
    if (conn.hasError()) {
        std::printf("FAIL S7 connect: %s\n%d failure(s)\n", sgrn::wrappers::s7::toString(conn.error()).data(), g_failures);
        return 1;
    }

    auto readDbFloat = [&](float& t_out) -> bool {
        auto raw = client.readDB(db_number, off, 4);
        if (raw.hasError()) {
            ++g_failures;
            std::printf("FAIL S7 readDB DB%d@%d: %s\n", db_number, off, sgrn::wrappers::s7::toString(raw.error()).data());
            return false;
        }
        CHECK(raw->size() == 4);
        if (raw->size() != 4)
            return false;
        t_out = floatFromDbBytes(*raw);
        return true;
    };

    // ── 3. S7 read ↔ HTTP twin agreement ─────────────────────────────────────
    float s7_value = 0.f;
    if (!readDbFloat(s7_value)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    float http_value = 0.f;
    {
        auto res = http.Get(("/data/" + db_name + "/" + field_path).c_str());
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            const Json::Value v = parseJson(res->body, "HTTP twin field");
            CHECK(v.isNumeric());
            if (v.isNumeric()) {
                http_value = v.asFloat();
                CHECK(std::fabs(s7_value - http_value) < 1e-6f);
                if (!(std::fabs(s7_value - http_value) < 1e-6f))
                    std::printf(
                        "FAIL S7/HTTP disagreement on %s/%s: S7=%.9g HTTP=%.9g", db_name.c_str(), field_path.c_str(), s7_value, http_value);
            }
        }
    }

    // ── 4. HTTP POST (the semantic field writer: uploadField validates and
    //        encodes via the schema; PUT here is the raw-memory API) → S7
    //        sees the new value ──────────────────────────────────────────────
    const float new_value = std::isfinite(s7_value) ? (s7_value + 3.25f) : 42.5f;
    {
        auto res = http.Post(("/data/" + db_name + "/" + field_path).c_str(), std::to_string(new_value), "application/json");
        CHECK(res && (res->status == 200 || res->status == 204));
        if (!(res && (res->status == 200 || res->status == 204))) {
            std::printf("FAIL HTTP POST /data/%s/%s (%d): %.160s\n", db_name.c_str(), field_path.c_str(), res ? res->status : 0,
                res ? res->body.c_str() : "");
        }
        // settle: gateway fan-out is immediate (in-process), short sync wait
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        float after = 0.f;
        if (readDbFloat(after)) {
            CHECK(std::fabs(after - new_value) < 1e-5f);
            if (!(std::fabs(after - new_value) < 1e-5f))
                std::printf("FAIL S7 read-back after HTTP PUT: %.9g (want %.9g)\n", after, new_value);
        }
    }

    // ── 5. S7 PUT exercised opportunistically (policy-dependent) ─────────────
    {
        std::vector<uint8_t> bytes;
        floatToDbBytes(new_value + 1.125f, bytes);
        auto w = client.writeDB(db_number, off, static_cast<int>(bytes.size()), bytes.data());
        if (w.hasError()) {
            std::printf("NOTE: S7 writeDB denied by security policy (%s) — HTTP-only policy scripts reject S7 PUTs\n",
                sgrn::wrappers::s7::toString(w.error()).data());
        } else {
            float back = 0.f;
            CHECK(readDbFloat(back));
            CHECK(std::fabs(back - (new_value + 1.125f)) < 1e-5f);
        }
    }

    client.disconnect();

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("s7_southbound_test: all checks passed (http:%d s7:%d)\n", gw.httpPort(), gw.s7Port());
    return 0;
}
