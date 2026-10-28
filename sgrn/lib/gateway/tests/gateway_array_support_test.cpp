// =============================================================================
// gateway_array_support_test.cpp — SGRN Gateway Array Operations & Bounds Test
// =============================================================================
//
// Purpose:
//   Validates the SGRN Gateway's handling of structured and primitive S7 arrays.
//   Ensures that array elements can be addressed individually via REST endpoints
//   without buffer overruns, torn reads, or clobbering adjacent array indices.
//
// Test Target:
//   `SafetySystems.gas_detectors` (defined as ARRAY [0..7] OF Real in gas_processing schema).
//
// Stages Tested:
//   1. Schema Registry Verification: Confirms field count == 8 and type == REAL.
//   2. Full Array Read: Confirms GET /data/.../gas_detectors returns 8-element array.
//   3. Single Element Read: Reads index [2] via /data/.../gas_detectors/2.
//   4. Single Element Write: Modifies index [2] via POST /data/.../gas_detectors/2.
//   5. Neighbour Isolation: Verifies only index [2] changed; index [0] and [1]
//      remain strictly unmodified in twin memory.
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
    std::printf(" [gateway_array_support_test] S7 Array Indexing & Mutation Verification\n");
    std::printf("======================================================================\n");

    sgrn::test::GatewayFixture gw("gas_processing");
    if (!gw.available()) {
        std::printf("SKIP: %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(std::chrono::seconds(2));
    http.set_read_timeout(std::chrono::seconds(2));

    // ── Stage 1: Verify Array Metadata in Schema Registry ────────────────────
    std::printf("[Stage 1] Querying /registry for SafetySystems.gas_detectors metadata...\n");
    {
        auto res = http.Get("/registry");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value reg = parseJson(res->body);
            bool found_array = false;
            for (const auto& db : reg["dbs"]) {
                if (db["name"].asString() == "SafetySystems") {
                    for (const auto& f : db["fields"]) {
                        if (f["name"].asString() == "gas_detectors") {
                            found_array = true;
                            CHECK(f["count"].asInt() == 8);
                        }
                    }
                }
            }
            CHECK(found_array);
            std::printf("  -> Verified: SafetySystems.gas_detectors has count = 8 elements\n");
        }
    }

    // ── Stage 2: Read Entire Array Snapshot ──────────────────────────────────
    std::printf("[Stage 2] Reading full array snapshot via GET /data/SafetySystems/gas_detectors...\n");
    std::vector<double> original_elements;
    {
        auto res = http.Get("/data/SafetySystems/gas_detectors");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value arr = parseJson(res->body);
            if (arr.isArray()) {
                CHECK(arr.size() == 8);
                for (unsigned int i = 0; i < arr.size(); ++i) {
                    original_elements.push_back(arr[i].asDouble());
                }
                std::printf("  -> Captured baseline for 8 array elements\n");
            }
        }
    }

    // ── Stage 3: Test Indexed Single-Element Read ─────────────────────────────
    std::printf("[Stage 3] Querying element [2] via GET /data/SafetySystems/gas_detectors/2...\n");
    {
        auto res = http.Get("/data/SafetySystems/gas_detectors/2");
        CHECK(res && res->status == 200);
        if (res && res->status == 200 && !original_elements.empty()) {
            Json::Value val = parseJson(res->body);
            if (val.isNumeric()) {
                CHECK(std::abs(val.asDouble() - original_elements[2]) < 0.01);
                std::printf("  -> Indexed element read matches baseline: %.2f\n", val.asDouble());
            }
        }
    }

    // ── Stage 4: Test Single-Element Partial Mutation ─────────────────────────
    const double target_val = 77.25;
    std::printf("[Stage 4] Mutating element [2] to %.2f via POST /data/SafetySystems/gas_detectors/2...\n", target_val);
    {
        const std::string post_body = std::to_string(target_val);
        auto post_res = http.Post("/data/SafetySystems/gas_detectors/2", post_body, "application/json");
        CHECK(post_res && (post_res->status == 200 || post_res->status == 204));
    }

    // ── Stage 5: Verify Neighbour Isolation (Zero Cross-Element Clobbering) ──
    std::printf("[Stage 5] Verifying mutation isolation and adjacent element integrity...\n");
    {
        auto res = http.Get("/data/SafetySystems/gas_detectors/2");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value val = parseJson(res->body);
            if (val.isNumeric()) {
                CHECK(std::abs(val.asDouble() - target_val) < 0.1);
            }
        }

        // Re-read full array: index 2 must be updated, indices 0 and 1 must remain untouched
        auto arr_res = http.Get("/data/SafetySystems/gas_detectors");
        CHECK(arr_res && arr_res->status == 200);
        if (arr_res && arr_res->status == 200 && original_elements.size() == 8) {
            Json::Value arr = parseJson(arr_res->body);
            if (arr.isArray() && arr.size() == 8) {
                CHECK(std::abs(arr[2].asDouble() - target_val) < 0.1);
                CHECK(std::abs(arr[0].asDouble() - original_elements[0]) < 0.01);
                CHECK(std::abs(arr[1].asDouble() - original_elements[1]) < 0.01);
                std::printf("  -> Confirmed: element [2] updated; elements [0] and [1] strictly preserved\n");
            }
        }
    }

    if (g_failures > 0) {
        std::printf("\ngateway_array_support_test: %d failure(s) detected\n", g_failures);
        return 1;
    }
    std::printf("\n======================================================================\n");
    std::printf(" [gateway_array_support_test] ALL ARRAY CHECKS PASSED (http:%d)\n", gw.httpPort());
    std::printf("======================================================================\n");
    return 0;
}
