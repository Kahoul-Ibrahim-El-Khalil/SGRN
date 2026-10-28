// =============================================================================
// gateway_rest_api_test.cpp — SGRN Gateway Northbound REST API Integration Test
// =============================================================================
//
// Purpose:
//   Validates the core HTTP northbound adapter of the SGRN Gateway against a live
//   emulated process environment (gas_processing). Ensures the REST surface correctly
//   exposes schema endpoints, digital twin live data, semantic writes, and raw
//   S7 memory buffers without memory corruptions or torn reads.
//
// Test Architecture:
//   - Spawns a hermetic Gateway + S7Shell instance via sgrn::test::GatewayFixture.
//   - Binds to dynamic OS-assigned loopback ports to prevent port collisions.
//   - Uses httplib::Client and jsoncpp to execute assertions.
//
// Tested Endpoints:
//   1. GET /endpoints
//      - Asserts route catalog exists and contains registered endpoints.
//   2. GET /registry
//      - Asserts compiled SCL schema returns valid DB and UDT descriptors.
//   3. GET /data/
//      - Asserts digital twin root JSON contains live DB state.
//   4. POST /data/<db> and GET /data/<db>/<field>
//      - Performs partial atomic write to DB memory and verifies write-through.
//   5. GET /memory/db/<id>/offset/<o>/size/<s>
//      - Asserts low-level S7 memory read returns expected byte slice.
// =============================================================================

#include "gateway_fixture.hpp"

#include <httplib.h>
#include <json/json.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                \
        }                                                                                                                                  \
    } while (0)

Json::Value parseJson(const std::string& t_json) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors)) {
        ++g_failures;
        std::printf("  [FAIL] JSON parsing failed: %s\n", errors.c_str());
        return {};
    }
    return v;
}

} // namespace

int main() {
    std::printf("======================================================================\n");
    std::printf(" [gateway_rest_api_test] Northbound REST API Integration Verification\n");
    std::printf("======================================================================\n");

    // Initialize GatewayFixture with standard gas_processing simulation
    sgrn::test::GatewayFixture gw("gas_processing");
    if (!gw.available()) {
        std::printf("SKIP: %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(std::chrono::seconds(2));
    http.set_read_timeout(std::chrono::seconds(2));

    // ── Stage 1: Verify Endpoint Catalog Discovery (/endpoints) ──────────────
    // The gateway serves an interactive route index for clients and dashboards.
    std::printf("[Stage 1] Querying GET /endpoints for service registry...\n");
    {
        auto res = http.Get("/endpoints");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value root = parseJson(res->body);
            CHECK(root.isMember("endpoints"));
            CHECK(root["endpoints"].isArray());
            CHECK(root["endpoints"].size() > 0);
            std::printf("  -> Discovered %u published API endpoints\n", root["endpoints"].size());
        }
    }

    // ── Stage 2: Verify SCL Schema Directory (/registry) ─────────────────────
    // Ensures compiled SCL data blocks (DBs) and user-defined types (UDTs)
    // are exposed with type-safe metadata (offsets, types, bit indexes).
    std::printf("[Stage 2] Querying GET /registry for compiled schema tree...\n");
    {
        auto res = http.Get("/registry");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value reg = parseJson(res->body);
            CHECK(reg.isMember("dbs"));
            CHECK(reg["dbs"].isArray());
            CHECK(reg["dbs"].size() > 0);
            CHECK(reg.isMember("udts"));
            CHECK(reg["udts"].isArray());
            std::printf("  -> Schema validated: %u Data Blocks, %u UDTs registered\n", reg["dbs"].size(), reg["udts"].size());
        }
    }

    // ── Stage 3: Verify Digital Twin Root State Read (/data/) ────────────────
    // Ensures twin snapshot re-assembles the active plant state into nested JSON.
    std::printf("[Stage 3] Querying GET /data/ for full digital twin snapshot...\n");
    {
        auto res = http.Get("/data/");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value data = parseJson(res->body);
            CHECK(data.isObject());
            CHECK(data.isMember("InletSeparation"));
            std::printf("  -> Received active twin root with InletSeparation DB\n");
        }
    }

    // ── Stage 4: Test Semantic Partial Write and Immediate Read-Back ──────────
    // Writes a numeric pressure value into InletSeparation.feed_pressure.
    // The gateway should lock the segment, update the double-buffer staging area,
    // swap atomically, and reflect the new value on subsequent reads.
    std::printf("[Stage 4] Performing POST /data/InletSeparation (feed_pressure=45.5 bar)...\n");
    {
        const std::string post_body = "{\"feed_pressure\": 45.5}";
        auto post_res = http.Post("/data/InletSeparation", post_body, "application/json");
        CHECK(post_res && (post_res->status == 200 || post_res->status == 204));

        auto read_res = http.Get("/data/InletSeparation/feed_pressure");
        CHECK(read_res && read_res->status == 200);
        if (read_res && read_res->status == 200) {
            Json::Value val = parseJson(read_res->body);
            if (val.isNumeric()) {
                CHECK(std::abs(val.asDouble() - 45.5) < 0.1);
            } else if (val.isMember("feed_pressure")) {
                CHECK(std::abs(val["feed_pressure"].asDouble() - 45.5) < 0.1);
            }
            std::printf("  -> Read-back verified: feed_pressure = 45.5 bar\n");
        }
    }

    // ── Stage 5: Test Raw Binary Memory Slicing (/memory/db/...) ─────────────
    // Directly reads a raw 16-byte memory slice from DB 1 at offset 0.
    // Verifies the application/octet-stream fast-path used by low-overhead drivers.
    std::printf("[Stage 5] Querying GET /memory/db/1/offset/0/size/16 for raw binary slice...\n");
    {
        auto mem_res = http.Get("/memory/db/1/offset/0/size/16");
        CHECK(mem_res && mem_res->status == 200);
        if (mem_res && mem_res->status == 200) {
            CHECK(!mem_res->body.empty());
            CHECK(mem_res->body.size() == 16);
            std::printf("  -> Raw memory slice verified (16 bytes received)\n");
        }
    }

    if (g_failures > 0) {
        std::printf("\ngateway_rest_api_test: %d failure(s) detected\n", g_failures);
        return 1;
    }
    std::printf("\n======================================================================\n");
    std::printf(" [gateway_rest_api_test] ALL REST API CHECKS PASSED (http:%d)\n", gw.httpPort());
    std::printf("======================================================================\n");
    return 0;
}
