// opcua_enum_test.cpp — #ENUM OPC-UA round-trip + rejected-write behaviour,
// ported from tests/gateway/enum_tests.py (python/asyncua) to the vendored
// open62541 client wrapper (sgrn::wrappers::opcua).
//
// Scope asserted here (the scalar-leaf and rejection semantics of the Python
// test; array/struct-array element browsing is exercised in reduced form):
//   1. Every scalar enum leaf of DB1 "EnumEdgeCases" accepts every declared
//      member through writeDataValue(ns=1;s=EnumEdgeCases.<path>) and reads
//      back the same numeric value (the wire/UA type is the registered
//      backing-width enum: BYTE / SBYTE / INT16 / UINT16 / INT32 kinds).
//   2. Undeclared values are REJECTED by the stack (BadInvalidArgument from
//      the leaf-write membership check in adapters/opcua/writeValue.cpp) —
//      the product contract the Python test expects.
//   3. Duplicate-signature dedup: "status" (USInt #ENUM) and "status_alias"
//      are distinct nodes with independent values.
//   4. Bare enum-typed scalar array (mode_log[0..9]) writes via an Int32Array
//      DataValue, reads back element-wise as an Int32 array, and cross-checks
//      the HTTP twin both whole (/data/EnumEdgeCases/mode_log → JSON array of
//      quoted labels) and indexed (/data/EnumEdgeCases/mode_log/<i>).
//   5. Regression control: plain non-enum field (counter) unaffected.
//   6. Type safety: a String DataValue to an enum node must not take effect.
//
// HTTP ground truth: the PlcMemory digital twin renders enum fields as their
// symbolic label (e.g. "FOUR"); the labels are taken from the /registry
// `enum` maps so the test never hardcodes them. A label or a raw number both
// satisfy the round-trip (labels are produced for declared members).
//
// Environment: gateway + s7shell spawned on OS-assigned ports by
// sgrn::test::GatewayFixture (simulation "enums"). Missing binaries → SKIP.

#include "gateway_fixture.hpp"

#include <sgrn/wrappers/opcua/Client.hpp>
#include <sgrn/wrappers/opcua/DataValue.hpp>
#include <sgrn/wrappers/opcua/NodeId.hpp>

#include <httplib.h>
#include <json/json.h>
#include <open62541/nodeids.h>
#include <open62541/types.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <sgrn/Result.hpp>

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

using sgrn::wrappers::opcua::Client;
using sgrn::wrappers::opcua::DataValue;
using sgrn::wrappers::opcua::NodeId;

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

/// Read an int scalar of any signed/unsigned backing width — enum nodes are
/// registered at their SCL base-type width (USInt → 1-byte BYTE kind,
/// Int → 2-byte INT16 kind, DInt → 4-byte INT32 kind, ...).
sgrn::Result<int64_t> readIntScalar(Client& t_client, const std::string& t_alias) {
    auto dv = t_client.readDataValue(NodeId::string(1, t_alias));
    if (dv.hasError())
        return sgrn::Result<int64_t>::Error("OPC-UA read " + t_alias + ": " + dv.error());
    const auto& raw = (*dv).get();
    if (!raw.hasValue || raw.value.data == nullptr || raw.value.type == nullptr)
        return sgrn::Result<int64_t>::Error("OPC-UA read " + t_alias + ": no scalar value");
    const UA_DataType* t_type = raw.value.type;
    if (t_type->typeKind == UA_DATATYPEKIND_ENUM || t_type->typeKind == UA_DATATYPEKIND_INT32) {
        if (t_type->memSize != sizeof(int32_t))
            return sgrn::Result<int64_t>::Error("OPC-UA read " + t_alias + ": odd enum width " + std::to_string(t_type->memSize));
        return static_cast<int64_t>(*static_cast<const int32_t*>(raw.value.data));
    }
    auto signedOf = [&](auto t_example) -> int64_t {
        using S = decltype(t_example);
        return static_cast<int64_t>(*static_cast<const S*>(raw.value.data));
    };
    switch (t_type->typeKind) {
        case UA_DATATYPEKIND_SBYTE:
            return signedOf(int8_t{});
        case UA_DATATYPEKIND_INT16:
            return signedOf(int16_t{});
        case UA_DATATYPEKIND_BYTE:
            return signedOf(uint8_t{});
        case UA_DATATYPEKIND_UINT16:
            return signedOf(uint16_t{});
        case UA_DATATYPEKIND_UINT32:
            return signedOf(uint32_t{});
        default:
            return sgrn::Result<int64_t>::Error("OPC-UA read " + t_alias + ": unexpected type kind " + std::to_string(t_type->typeKind));
    }
}

class LabelMap {
public:
    explicit LabelMap(const Json::Value& t_db_fields) {
        for (const auto& f : t_db_fields)
            collect(f, "");
    }

    const std::string* labelFor(const std::string& t_path, int64_t t_value) const {
        const auto path_it = m_by_path.find(t_path);
        if (path_it == m_by_path.end())
            return nullptr;
        const auto val_it = path_it->second.find(static_cast<int>(t_value));
        return val_it == path_it->second.end() ? nullptr : &val_it->second;
    }

private:
    void collect(const Json::Value& t_field, const std::string& t_prefix) {
        const std::string name = t_field["name"].asString();
        const std::string path = t_prefix.empty() ? name : t_prefix + "." + name;
        if (t_field.isMember("enum") && t_field["enum"].isObject()) {
            std::map<int, std::string> by_value;
            for (auto it = t_field["enum"].begin(); it != t_field["enum"].end(); ++it)
                by_value[std::atoi(it.key().asCString())] = it->asString();
            if (!by_value.empty())
                m_by_path[path] = std::move(by_value);
        }
        if (t_field.isMember("children") && t_field["children"].isArray())
            for (const auto& child : t_field["children"])
                collect(child, path);
    }

    std::map<std::string, std::map<int, std::string>> m_by_path;
};

} // namespace

int main() {
    sgrn::test::GatewayFixture gw("enums");
    if (!gw.available()) {
        std::printf("SKIP: gateway unavailable — %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(3);

    // Registry labels (ground truth rendering for the twin) — DB displayed
    // name comes from the /registry directory itself.
    auto reg_res = http.Get("/registry");
    CHECK(reg_res && reg_res->status == 200);
    if (!(reg_res && reg_res->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value doc = parseJson(reg_res->body, "/registry");
    const Json::Value& dbs = doc["dbs"];
    CHECK(dbs.isArray() && !dbs.empty());
    if (!(dbs.isArray() && !dbs.empty())) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const std::string db_name = dbs[0]["name"].asString();
    LabelMap labels(dbs[0]["fields"]);

    // OPC-UA client: fresh session on the OS-assigned port
    auto connect = sgrn::wrappers::opcua::Client::connect({.endpoint_url = "opc.tcp://127.0.0.1:" + std::to_string(gw.opcuaPort())});
    CHECK(!connect.hasError());
    if (connect.hasError()) {
        std::printf("FAIL connect: %s\n%d failure(s)\n", connect.error().c_str(), g_failures);
        return 1;
    }
    Client client = std::move(*connect);

    const std::string alias_prefix = db_name + ".";

    /// Write the value, read back over OPC-UA, and independently cross-check
    /// the HTTP twin — a passing OPC-UA read alone must not hide a write that
    /// landed somewhere else. The twin renders a declared enum member as its
    /// symbolic label (JSON string); an undeclared member falls back to the
    /// raw number. `t_http_path` is empty for paths the REST resolver cannot
    /// address (nested struct members).
    auto writeAndVerify = [&](const std::string& t_field, int32_t t_value, const std::string& t_http_path) -> void {
        const std::string alias = alias_prefix + t_field;
        const std::string name = "write " + alias + " = " + std::to_string(t_value);

        if (const auto w = client.writeDataValue(NodeId::string(1, alias), DataValue::fromInt32(t_value)); w.hasError()) {
            ++g_failures;
            std::printf("FAIL %s: unexpected write failure: %s\n", name.c_str(), w.error().c_str());
            return;
        }

        const auto back = readIntScalar(client, alias);
        if (!back.hasValue() || *back != t_value) {
            ++g_failures;
            std::printf("FAIL %s: read-back %s\n", name.c_str(), back.hasValue() ? std::to_string(*back).c_str() : back.error().c_str());
            return;
        }

        if (t_http_path.empty())
            return;

        auto res = http.Get(("/data/" + db_name + "/" + t_http_path).c_str());
        if (!res || res->status != 200) {
            ++g_failures;
            std::printf(
                "FAIL %s: HTTP GET /data/%s/%s failed (%d)\n", name.c_str(), db_name.c_str(), t_http_path.c_str(), res ? res->status : 0);
            return;
        }
        // The twin's leaf response is a bare JSON scalar without surrounding
        // decoration — string labels come back QUOTED (e.g. `"ONE"`), which
        // jsoncpp accepts; numbers parse fine. Compare raw-token after a
        // best-effort JSON parse (strips quotes when they are present).
        std::string body = res->body;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
            body.pop_back();
        while (!body.empty() && (body.front() == ' '))
            body.erase(body.begin());
        const std::string std_expect = labels.labelFor(t_field, static_cast<int64_t>(t_value))
                                           ? *labels.labelFor(t_field, static_cast<int64_t>(t_value))
                                           : std::to_string(t_value);
        std::string token = body;
        if (!body.empty() && body.front() == '"' && body.back() == '"')
            token = body.substr(1, body.size() >= 2 ? body.size() - 2 : 0);
        if (token != std_expect && body != std::to_string(t_value)) {
            ++g_failures;
            std::printf("FAIL %s: HTTP read-back %.200s (want %s)\n", name.c_str(), res->body.c_str(), std_expect.c_str());
        }
    };

    /// Undeclared member: the write must be REJECTED by the stack now that
    /// the leaf-write membership validation guards the enum path.
    auto writeExpectRejected = [&](const std::string& t_field, int32_t t_value) -> void {
        const std::string name = "reject write " + alias_prefix + t_field + " = " + std::to_string(t_value);
        if (const auto w = client.writeDataValue(NodeId::string(1, alias_prefix + t_field), DataValue::fromInt32(t_value)); !w.hasError()) {
            ++g_failures;
            std::printf("FAIL %s: undeclared write accepted\n", name.c_str());
        }
    };

    // ── 1. Baseline + every #ENUM flavour: declared members round-trip ─────
    for (const int32_t v : {0, 1, 2, 3}) // "Status" USInt shorthand
        writeAndVerify("status", v, "status");

    // ── 2. Native TIA syntax incl. auto-incremented member (6) ─────────────
    for (const int32_t v : {0, 3, 5, 6})
        writeAndVerify("plant_mode", v, "plant_mode");
    writeExpectRejected("plant_mode", 7); // UKNOWN+1 undeclared

    // ── 3. Negative values (SInt-backed) ───────────────────────────────────
    for (const int32_t v : {-1, 0, 1})
        writeAndVerify("sign_bit", v, "sign_bit");
    writeExpectRejected("sign_bit", -2); // in-range for SInt, undeclared
    writeExpectRejected("sign_bit", 2);

    // ── 4. Full 32-bit boundary values (DInt-backed) ────────────────────────
    for (const int32_t v : {-2000000000, 0, 2000000000})
        writeAndVerify("wide_enum", v, "wide_enum");
    writeExpectRejected("wide_enum", -2147483648); // INT32 min, undeclared
    writeExpectRejected("wide_enum", 2000000001);

    // ── 5. Unsigned byte boundary — 255 must not read as negative ──────────
    for (const int32_t v : {0, 127, 255})
        writeAndVerify("byte_max", v, "byte_max");
    writeExpectRejected("byte_max", 256);

    // ── 6. Sparse set: membership check, not width/range alone ─────────────
    for (const int32_t v : {10, 20, 21, 1000})
        writeAndVerify("sparse_gap", v, "sparse_gap");
    writeExpectRejected("sparse_gap", 500);
    writeExpectRejected("sparse_gap", 11);

    // ── 8. Inline field-level #ENUM (no named UDT alias) ───────────────────
    for (const int32_t v : {0, 1, 2})
        writeAndVerify("inline_priority", v, "inline_priority");

    // ── 11. Nested UDT-of-UDT, enum two levels deep (OPC-UA only; the REST
    //        path resolver has no dotted-slash form for struct members)
    writeAndVerify("station.mode", 2, "");
    writeAndVerify("station.last_alarm.priority", 3, "");

    // ── 7. Duplicate-signature dedup: same signatures across distinct SCL
    //       types must still be independent nodes.
    writeAndVerify("status", 1, "status");
    writeAndVerify("status_alias", 3, "status_alias");
    {
        const auto s1 = readIntScalar(client, alias_prefix + "status");
        const auto s2 = readIntScalar(client, alias_prefix + "status_alias");
        CHECK(s1.hasValue() && s2.hasValue() && *s1 == 1 && *s2 == 3);
        if (!(s1.hasValue() && s2.hasValue() && *s1 == 1 && *s2 == 3))
            std::printf("FAIL dedup independence: status=%s status_alias=%s\n",
                s1.hasValue() ? std::to_string(*s1).c_str() : s1.error().c_str(),
                s2.hasValue() ? std::to_string(*s2).c_str() : s2.error().c_str());
    }

    // ── 9/10. Array of a bare enum-typed scalar alias (mode_log[0..9]) ─────
    //          Write + read back an Int32Array over the node (enum arrays
    //          project as Int32 per OPC UA Part 6 §5.1.5, matching the scalar
    //          leaf projection); HTTP twin ground truth both whole
    //          (/data/<DB>/mode_log → JSON array of quoted labels) and
    //          indexed (/data/<DB>/mode_log/<i> → one quoted label).
    {
        std::vector<int32_t> want(10);
        for (int i = 0; i < 10; ++i)
            want[static_cast<std::size_t>(i)] = i % 7; // cycles every PlantModeNative member incl. auto UKNOWN=6
        if (const auto w = client.writeDataValue(NodeId::string(1, alias_prefix + "mode_log"), DataValue::fromInt32Array(want));
            w.hasError()) {
            ++g_failures;
            std::printf("FAIL array write mode_log: %s\n", w.error().c_str());
        } else {
            // Int32-payload array read-back (builtin Int32 or a custom Enum
            // DataType with 4-byte members — both carry Int32 elements).
            const auto back = client.readDataValue(NodeId::string(1, alias_prefix + "mode_log"));
            if (back.hasError()) {
                ++g_failures;
                std::printf("FAIL array OPC read mode_log: %s\n", back.error().c_str());
            } else {
                const auto& raw = (*back).get();
                const bool shape_ok = raw.hasValue && raw.value.data != nullptr && raw.value.type != nullptr &&
                                      raw.value.arrayLength == 10 && raw.value.type->memSize == sizeof(int32_t);
                CHECK(shape_ok);
                if (shape_ok) {
                    const auto* p_elems = static_cast<const int32_t*>(raw.value.data);
                    for (size_t i = 0; i < 10; ++i)
                        if (p_elems[i] != want[i]) {
                            ++g_failures;
                            std::printf("FAIL OPC mode_log[%zu]: %d (want %d)\n", i, p_elems[i], want[i]);
                        }
                } else if (raw.hasValue && raw.value.type != nullptr) {
                    std::printf("note mode_log array readback: typeKind=%d memSize=%u arrayLength=%u\n",
                        static_cast<int>(raw.value.type->typeKind), static_cast<unsigned>(raw.value.type->memSize),
                        static_cast<unsigned>(raw.value.arrayLength));
                }
            }

            // HTTP whole-array: labels for every declared member cycle.
            auto res = http.Get("/data/" + db_name + "/mode_log");
            if (res && res->status == 200) {
                const Json::Value arr = parseJson(res->body, "HTTP twin enum array");
                const bool ok = arr.isArray() && arr.size() == want.size();
                CHECK(ok);
                for (int i = 0; i < 10 && i < (int)arr.size(); ++i) {
                    const std::string* want_label = labels.labelFor("mode_log", want[static_cast<std::size_t>(i)]);
                    const std::string got = arr[i].isString() ? arr[i].asString() : arr[i].toStyledString();
                    if (want_label && got != *want_label) {
                        ++g_failures;
                        std::printf("FAIL HTTP mode_log[%d]: %.32s (want %s)\n", i, got.c_str(), want_label->c_str());
                    }
                }
            } else {
                ++g_failures;
                std::printf("FAIL HTTP GET mode_log failed (%d)\n", res ? res->status : 0);
            }

            // HTTP indexed element read: the handler resolves it through the
            // whole-array JSON (same labels as above).
            for (const int i : {0, 4, 9}) {
                auto r = http.Get(("/data/" + db_name + "/mode_log/" + std::to_string(i)).c_str());
                if (!r || r->status != 200) {
                    ++g_failures;
                    std::printf("FAIL HTTP GET mode_log/%d failed (%d)\n", i, r ? r->status : 0);
                    continue;
                }
                const Json::Value el = parseJson(r->body, "HTTP indexed enum element");
                const std::string* want_label = labels.labelFor("mode_log", want[static_cast<std::size_t>(i)]);
                if (!el.isString()) {
                    ++g_failures;
                    std::printf("FAIL HTTP mode_log/%d: expected a label string, got %.40s\n", i, r->body.c_str());
                } else if (want_label && el.asString() != *want_label) {
                    ++g_failures;
                    std::printf("FAIL HTTP mode_log/%d: %s (want %s)\n", i, el.asString().c_str(), want_label->c_str());
                }
            }
        }
    }

    // ── 12. Regression control: plain non-enum field unaffected ────────────
    writeAndVerify("counter", 424242, "counter");

    // ── 13. Type safety: a String DataValue to an enum node must not take
    //        effect (stack type mismatch or no-op, never silent coercion).
    {
        const auto before = readIntScalar(client, alias_prefix + "status");
        if (before.hasValue()) {
            if (const auto w = client.writeDataValue(NodeId::string(1, alias_prefix + "status"), DataValue::fromString("RUNNING"));
                !w.hasError()) {
                const auto after = readIntScalar(client, alias_prefix + "status");
                CHECK(after.hasValue() && *after == *before);
            } // else: the stack rejected the mismatching type — also correct
        }
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("opcua_enum_test: all checks passed (http:%d opcua:%d)\n", gw.httpPort(), gw.opcuaPort());
    return 0;
}
