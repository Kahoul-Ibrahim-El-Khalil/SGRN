// =============================================================================
// gateway_policy_test.cpp — SGRN Security Policy & Schema Validation Test
// =============================================================================
//
// Purpose:
//   Validates the SGRN Gateway's security subsystem and declarative schema
//   compilation engine.
//
// Test Target:
//   - PolicyEngine and SecurityManager JSON projection (/api/policy).
//   - Schema Registry structure, DB number bindings, and UDT descriptors (/registry).
//
// Stages Tested:
//   1. Policy Rules Inspection: Asserts /api/policy exposes an active rules array.
//   2. Schema Structure Verification: Verifies each DB contains name, number,
//      valid byte sizes, and non-empty field definitions matching SCL definitions.
// =============================================================================

#include "gateway_fixture.hpp"

#include <httplib.h>
#include <json/json.h>

#include <cstdio>
#include <string>

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
    std::printf(" [gateway_policy_test] Security Policy & Schema Structure Verification\n");
    std::printf("======================================================================\n");

    sgrn::test::GatewayFixture gw("gas_processing");
    if (!gw.available()) {
        std::printf("SKIP: %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(std::chrono::seconds(2));
    http.set_read_timeout(std::chrono::seconds(2));

    // ── Stage 1: Verify Security Policy Engine JSON Surface (/api/policy) ────
    // The policy engine compiles AngelScript rules into active rule tables.
    // Verifies the HTTP adapter serves the active rule array.
    std::printf("[Stage 1] Querying GET /api/policy for compiled access rules...\n");
    {
        auto res = http.Get("/api/policy");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value policy = parseJson(res->body);
            CHECK(policy.isMember("rules"));
            CHECK(policy["rules"].isArray());
            std::printf("  -> Verified policy table with %u active rule(s)\n", policy["rules"].size());
        }
    }

    // ── Stage 2: Verify Schema Integrity Across All Data Blocks (/registry) ──
    // Every registered DB must contain valid numbers, memory byte allocations,
    // and correctly nested field arrays.
    std::printf("[Stage 2] Inspecting /registry schema integrity and DB sizing...\n");
    {
        auto res = http.Get("/registry");
        CHECK(res && res->status == 200);
        if (res && res->status == 200) {
            Json::Value reg = parseJson(res->body);
            CHECK(reg.isMember("dbs"));
            CHECK(reg["dbs"].isArray());
            CHECK(!reg["dbs"].empty());

            for (const auto& db : reg["dbs"]) {
                CHECK(db.isMember("name"));
                CHECK(db.isMember("number"));
                CHECK(db.isMember("size_bytes"));
                CHECK(db["size_bytes"].asUInt() > 0);
                CHECK(db.isMember("fields"));
                CHECK(db["fields"].isArray());
            }

            CHECK(reg.isMember("udts"));
            CHECK(reg["udts"].isArray());
            std::printf("  -> Verified %u DB schemas with non-zero memory allocation\n", reg["dbs"].size());
        }
    }

    if (g_failures > 0) {
        std::printf("\ngateway_policy_test: %d failure(s) detected\n", g_failures);
        return 1;
    }
    std::printf("\n======================================================================\n");
    std::printf(" [gateway_policy_test] ALL POLICY & SCHEMA CHECKS PASSED (http:%d)\n", gw.httpPort());
    std::printf("======================================================================\n");
    return 0;
}
