// opcua_discovery_test.cpp — live gateway OPC-UA discovery, ported from
// tests/gateway/opcua_discovery.py (python/asyncua) to the vendored
// open62541 client wrapper (sgrn::wrappers::opcua).
//
// Scope asserted here (the Python file doubles as a tree *inspector*; this
// test keeps the deterministic assertions):
//   1. GET /registry over HTTP returns the schema directory; every entry has
//      `number`, `name`, `size_bytes` and a `fields` array.
//   2. Connecting from a fresh client to opc.tcp://127.0.0.1:<random port>
//      works; the NamespaceArray has at least the standard + gateway
//      namespaces (ns=0, ns=1).
//   3. Every registry DB is a live Object node directly under the standard
//      ObjectsFolder (ns=0) with NodeId ns=1;s=<db_name>.
//   4. Each DB's DBNumber property (NodeId ns=1;s=<name>.DBNumber — qualified
//      name ns=0 per information_model.cpp) reads back as a UInt16 equal to
//      the registry `number`.
//
// Environment: gateway + s7shell spawned on OS-assigned ports by
// sgrn::test::GatewayFixture; SGRN_SIMULATION selects the simulation.
// Missing binaries → SKIP + exit 0.

#include "gateway_fixture.hpp"

#include <sgrn/wrappers/opcua/Client.hpp>
#include <sgrn/wrappers/opcua/NodeId.hpp>

#include <httplib.h>
#include <json/json.h>
#include <open62541/nodeids.h>
#include <open62541/types.h>

#include <algorithm>
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
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

Json::Value parseJson(const std::string& t_json) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors)) {
        ++g_failures;
        std::printf("FAIL /registry JSON does not parse: %s (%.160s)\n", errors.c_str(), t_json.c_str());
        return {};
    }
    return v;
}

} // namespace

int main() {
    sgrn::test::GatewayFixture gw;
    if (!gw.available()) {
        std::printf("SKIP: gateway unavailable — %s\n", gw.skipReason().c_str());
        return 0;
    }

    // ── 1. HTTP registry (the schema directory DBs) ─────────────────────────
    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(3);
    auto body = http.Get("/registry");
    CHECK(body && body->status == 200);
    if (!(body && body->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value doc = parseJson(body->body);
    const Json::Value& dbs = doc["dbs"];
    CHECK(dbs.isArray() && !dbs.empty());
    if (!(dbs.isArray() && !dbs.empty())) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    for (const auto& db : dbs) {
        CHECK(db.isMember("number") && db["number"].isIntegral());
        CHECK(db.isMember("name") && db["name"].isString());
        CHECK(db.isMember("size_bytes") && db["size_bytes"].isIntegral());
        CHECK(db.isMember("fields") && db["fields"].isArray());
    }

    // ── 2–4. OPC-UA client: connect, NamespaceArray, DB objects + DBNumber ──
    {
        auto connect = sgrn::wrappers::opcua::Client::connect({.endpoint_url = "opc.tcp://127.0.0.1:" + std::to_string(gw.opcuaPort())});
        CHECK(!connect.hasError());
        if (connect.hasError()) {
            std::printf("FAIL connect: %s\n", connect.error().c_str());
            std::printf("%d failure(s)\n", g_failures);
            return 1;
        }
        sgrn::wrappers::opcua::Client client = std::move(*connect);

        // NamespaceArray (under the Server object): at least standard +
        // gateway namespaces
        {
            auto ns = client.readDataValue(sgrn::wrappers::opcua::NodeId::numeric(0, UA_NS0ID_SERVER_NAMESPACEARRAY));
            CHECK(!ns.hasError());
            if (ns.hasError())
                std::printf("FAIL NamespaceArray read: %s\n", ns.error().c_str());
            else {
                const auto& raw = (*ns).get();
                CHECK(raw.hasValue);
                CHECK(raw.value.type == &UA_TYPES[UA_TYPES_STRING]);
                CHECK(raw.value.arrayLength >= 2); // ns=0 standard + ns=1 gateway
            }
        }

        // ObjectsFolder holds one Object per DB
        std::vector<sgrn::wrappers::opcua::NodeId> objects_children;
        {
            auto kids = client.browse(sgrn::wrappers::opcua::NodeId::numeric(0, UA_NS0ID_OBJECTSFOLDER));
            CHECK(!kids.hasError());
            if (kids.hasError())
                std::printf("FAIL ObjectsFolder browse: %s\n", kids.error().c_str());
            else
                objects_children = std::move(*kids);
        }
        CHECK(!objects_children.empty());

        // DBNumber properties read back as UInt16 == registry number
        for (const auto& db : dbs) {
            const std::string name = db["name"].asString();
            const uint16_t number = static_cast<uint16_t>(db["number"].asUInt());

            const sgrn::wrappers::opcua::NodeId db_object = sgrn::wrappers::opcua::NodeId::string(1, name);
            const bool present = std::any_of(objects_children.begin(), objects_children.end(),
                [&db_object](const sgrn::wrappers::opcua::NodeId& t_child) { return t_child == db_object; });
            CHECK(present);

            auto prop = client.readDataValue(sgrn::wrappers::opcua::NodeId::string(1, name + ".DBNumber"));
            CHECK(!prop.hasError());
            if (prop.hasError())
                std::printf("FAIL DBNumber read for DB '%s': %s\n", name.c_str(), prop.error().c_str());
            else {
                const auto& raw = (*prop).get();
                CHECK(raw.hasValue);
                CHECK(raw.value.type == &UA_TYPES[UA_TYPES_UINT16]);
                CHECK(raw.value.data != nullptr && *static_cast<const UA_UInt16*>(raw.value.data) == number);
            }
        }
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("opcua_discovery_test: all checks passed (http:%d opcua:%d)\n", gw.httpPort(), gw.opcuaPort());
    return 0;
}
