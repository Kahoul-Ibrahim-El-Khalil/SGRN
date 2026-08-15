#include <sgrn/gateway/security/PolicyEngine.hpp>

#include <fmt/core.h>
#include <sgrn/utils/strings.hpp>
#include <scriptarray/scriptarray.h>
#include <scriptbuilder/scriptbuilder.h>

#include <sgrn/utils/network.hpp>
#include <algorithm>
#include <arpa/inet.h>

namespace sgrn::gateway::security
{

// ── RuleTable ────────────────────────────────────────────────────────────────

void RuleTable::addRule(Rule t_r) {
    // Compute specificity score: higher = evaluated first
    int spec = 0;
    for (auto& t_c : t_r.allow_cidrs) {
        uint32_t m = t_c.mask;
        while (m) {
            spec += (m & 1);
            m >>= 1;
        } // popcount
    }
    if (!t_r.any_db)
        spec += 10;
    if (!t_r.allow_origins.empty())
        spec += 5;
    spec += static_cast<int>(t_r.require_headers.size()) * 3;
    if (!t_r.allow_session_names.empty())
        spec += 4;
    spec += static_cast<int>(t_r.field_rules.size()) * 20; // field rules are very specific
    t_r.specificity = spec;

    // Insert in descending specificity order (stable: same-spec keeps insertion order)
    auto& bucket = rules_by_protocol_[static_cast<size_t>(t_r.protocol)];
    auto it = std::lower_bound(
        bucket.begin(), bucket.end(), t_r, [](const Rule& t_a, const Rule& t_b) { return t_a.specificity > t_b.specificity; });
    bucket.insert(it, std::move(t_r));
}

void RuleTable::clear() {
    for (auto& bucket : rules_by_protocol_) {
        bucket.clear();
    }
}

bool RuleTable::authorize(const RequestContext& t_ctx) const {
    const auto& bucket = rules_by_protocol_[static_cast<size_t>(t_ctx.protocol)];
    if (bucket.empty())
        return !configured_protocols_.count(t_ctx.protocol);

    sgrn::utils::network::NetworkResult<uint32_t> client_ipv4 = parseIpv4Strict(t_ctx.client_ip);

    for (const auto& rule : bucket) {
        // IP/CIDR
        if (!rule.allow_cidrs.empty()) {
            if (client_ipv4.hasError()) {
                continue;
            }
            bool ok = false;
            for (auto& t_c : rule.allow_cidrs)
                if (t_c.match(client_ipv4.value())) {
                    ok = true;
                    break;
                }
            if (!ok)
                continue;
        }

        // DB (S7/Modbus/EIP/HTTP)
        if (!rule.any_db) {
            if (!t_ctx.db_number.has_value())
                continue;
            bool ok = false;
            for (auto db : rule.allow_db_numbers)
                if (db == *t_ctx.db_number) {
                    ok = true;
                    break;
                }
            if (!ok)
                continue;
        }

        // Origin (HTTP/WS)
        if (!rule.allow_origins.empty()) {
            bool ok = false;
            for (auto& o : rule.allow_origins)
                if (o == t_ctx.http_origin) {
                    ok = true;
                    break;
                }
            if (!ok)
                continue;
        }

        // Required headers — AND semantics
        if (!rule.require_headers.empty()) {
            bool ok = true;
            for (auto& hdr : rule.require_headers) {
                bool found = false;
                for (auto& ph : t_ctx.present_headers)
                    if (ph == hdr) {
                        found = true;
                        break;
                    }
                if (!found) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
        }

        // OPC-UA session name
        if (!rule.allow_session_names.empty()) {
            bool ok = false;
            for (auto& s : rule.allow_session_names)
                if (s == t_ctx.opc_session_name) {
                    ok = true;
                    break;
                }
            if (!ok)
                continue;
        }

        // Field rules (Part 2)
        if (!rule.field_rules.empty()) {
            if (t_ctx.field_path.empty()) {
                // Request is whole-DB, but rule specifies field-level restrictions -> deny
                continue;
            }
            bool field_ok = false;
            for (const auto& fr : rule.field_rules) {
                if (sgrn::utils::strings::fieldPathMatches(fr.field_pattern, t_ctx.field_path)) {
                    if (t_ctx.is_write && fr.allow_write)
                        field_ok = true;
                    else if (!t_ctx.is_write && fr.allow_read)
                        field_ok = true;
                    if (field_ok)
                        break;
                }
            }
            if (!field_ok)
                continue; // no matching field rule found or operation not allowed
        }

        return rule.allow; // first match wins
    }

    // Configured protocols that did not match any rule return false (deny-by-default)
    return !configured_protocols_.count(t_ctx.protocol);
}

// ── PolicyBuilder ─────────────────────────────────────────────────────────────

PolicyBuilder* PolicyBuilder::allowIp(const std::string& t_ip) {
    auto m = parseIp(t_ip);
    if (m)
        pending_.allow_cidrs.push_back(*m);
    else
        fmt::print(stderr, "[policy] Invalid IP '{}' — skipped\n", t_ip);
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowCidr(const std::string& t_cidr) {
    auto m = parseCidr(t_cidr);
    if (m)
        pending_.allow_cidrs.push_back(*m);
    else
        fmt::print(stderr, "[policy] Invalid CIDR '{}' — skipped\n", t_cidr);
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowDb(int t_db_number) {
    pending_.allow_db_numbers.push_back(static_cast<uint16_t>(t_db_number));
    pending_.any_db = false;
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowDbArray(CScriptArray* tp_dbs) {
    if (tp_dbs) {
        for (asUINT i = 0; i < tp_dbs->GetSize(); i++) {
            int t_db_number = *static_cast<int*>(tp_dbs->At(i));
            pending_.allow_db_numbers.push_back(static_cast<uint16_t>(t_db_number));
        }
        pending_.any_db = false;
        tp_dbs->Release();
    }
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowOrigin(const std::string& t_origin) {
    pending_.allow_origins.push_back(t_origin);
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::requireHeader(const std::string& t_header_name) {
    pending_.require_headers.push_back(t_header_name);
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowSession(const std::string& t_session_name) {
    pending_.allow_session_names.push_back(t_session_name);
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowField(const std::string& t_pattern) {
    pending_.field_rules.push_back({t_pattern, true, true});
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowFieldArray(CScriptArray* tp_patterns) {
    if (tp_patterns) {
        for (asUINT i = 0; i < tp_patterns->GetSize(); i++) {
            std::string* p_p = static_cast<std::string*>(tp_patterns->At(i));
            pending_.field_rules.push_back({*p_p, true, true});
        }
        tp_patterns->Release();
    }
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowFieldRead(const std::string& t_pattern) {
    pending_.field_rules.push_back({t_pattern, true, false});
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::allowFieldReadArray(CScriptArray* tp_patterns) {
    if (tp_patterns) {
        for (asUINT i = 0; i < tp_patterns->GetSize(); i++) {
            std::string* p_p = static_cast<std::string*>(tp_patterns->At(i));
            pending_.field_rules.push_back({*p_p, true, false});
        }
        tp_patterns->Release();
    }
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::denyFieldWrite(const std::string& t_pattern) {
    pending_.field_rules.push_back({t_pattern, true, false});
    addRef();
    return this;
}

PolicyBuilder* PolicyBuilder::denyFieldWriteArray(CScriptArray* tp_patterns) {
    if (tp_patterns) {
        for (asUINT i = 0; i < tp_patterns->GetSize(); i++) {
            std::string* p_p = static_cast<std::string*>(tp_patterns->At(i));
            pending_.field_rules.push_back({*p_p, true, false});
        }
        tp_patterns->Release();
    }
    addRef();
    return this;
}

void PolicyBuilder::commit(bool t_allow_action) {
    pending_.allow = t_allow_action;
    table_->addRule(pending_);
    pending_ = Rule{};             // reset for potential re-use
    pending_.protocol = protocol_; // preserve protocol for chaining
}

// ── AS factory helpers ────────────────────────────────────────────────────────
// File-scope staging pointer — set/cleared by PolicyEngine::loadScript().
// This is safe because loadScript() always runs single-threaded at boot/reload.

static RuleTable* p_g_staging_table = nullptr;

static PolicyBuilder* factory_make(Protocol t_proto) {
    if (p_g_staging_table)
        p_g_staging_table->markProtocolConfigured(t_proto);
    auto* p_b = new PolicyBuilder(p_g_staging_table, t_proto);
    return p_b;
}

static PolicyBuilder* factory_s7() {
    return factory_make(Protocol::S7);
}
static PolicyBuilder* factory_http() {
    return factory_make(Protocol::HTTP);
}
static PolicyBuilder* factory_ws() {
    return factory_make(Protocol::WebSocket);
}
static PolicyBuilder* factory_modbus() {
    return factory_make(Protocol::Modbus);
}
static PolicyBuilder* factory_opcua() {
    return factory_make(Protocol::OpcUA);
}
static PolicyBuilder* factory_eip() {
    return factory_make(Protocol::EthernetIP);
}

// ── allow() / deny() thunk wrappers ─────────────────────────────────────────
// AS can't distinguish overloads by argument value; we expose two named methods.
static void pb_allow(PolicyBuilder* tp_pb) {
    tp_pb->commit(true);
}
static void pb_deny(PolicyBuilder* tp_pb) {
    tp_pb->commit(false);
}

// ── Binding registration ──────────────────────────────────────────────────────

void registerPolicyBindings(sgrn::scripting::ScriptHost& t_host, RuleTable* tp_staging_table) {
    p_g_staging_table = tp_staging_table;
    asIScriptEngine* p_engine = t_host.getEngine();

    p_engine->RegisterObjectType("PolicyBuilder", 0, asOBJ_REF);
    p_engine->RegisterObjectBehaviour("PolicyBuilder", asBEHAVE_ADDREF, "void f()", asMETHOD(PolicyBuilder, addRef), asCALL_THISCALL);
    p_engine->RegisterObjectBehaviour("PolicyBuilder", asBEHAVE_RELEASE, "void f()", asMETHOD(PolicyBuilder, release), asCALL_THISCALL);

    // Fluent builder methods — return PolicyBuilder@ for chaining
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowIp(const string &in)", asMETHOD(PolicyBuilder, allowIp), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowCidr(const string &in)", asMETHOD(PolicyBuilder, allowCidr), asCALL_THISCALL);
    p_engine->RegisterObjectMethod("PolicyBuilder", "PolicyBuilder@ allowDb(int)", asMETHOD(PolicyBuilder, allowDb), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowDb(array<int>@)", asMETHOD(PolicyBuilder, allowDbArray), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowOrigin(const string &in)", asMETHOD(PolicyBuilder, allowOrigin), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ requireHeader(const string &in)", asMETHOD(PolicyBuilder, requireHeader), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowSession(const string &in)", asMETHOD(PolicyBuilder, allowSession), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowField(const string &in)", asMETHOD(PolicyBuilder, allowField), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowField(array<string>@)", asMETHOD(PolicyBuilder, allowFieldArray), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowFieldRead(const string &in)", asMETHOD(PolicyBuilder, allowFieldRead), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ allowFieldRead(array<string>@)", asMETHOD(PolicyBuilder, allowFieldReadArray), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ denyFieldWrite(const string &in)", asMETHOD(PolicyBuilder, denyFieldWrite), asCALL_THISCALL);
    p_engine->RegisterObjectMethod(
        "PolicyBuilder", "PolicyBuilder@ denyFieldWrite(array<string>@)", asMETHOD(PolicyBuilder, denyFieldWriteArray), asCALL_THISCALL);
    p_engine->RegisterObjectMethod("PolicyBuilder", "void allow()", asFUNCTION(pb_allow), asCALL_CDECL_OBJFIRST);
    p_engine->RegisterObjectMethod("PolicyBuilder", "void deny()", asFUNCTION(pb_deny), asCALL_CDECL_OBJFIRST);

    // Protocol factory globals
    p_engine->RegisterGlobalFunction("PolicyBuilder@ s7()", asFUNCTION(factory_s7), asCALL_CDECL);
    p_engine->RegisterGlobalFunction("PolicyBuilder@ http()", asFUNCTION(factory_http), asCALL_CDECL);
    p_engine->RegisterGlobalFunction("PolicyBuilder@ ws()", asFUNCTION(factory_ws), asCALL_CDECL);
    p_engine->RegisterGlobalFunction("PolicyBuilder@ modbus()", asFUNCTION(factory_modbus), asCALL_CDECL);
    p_engine->RegisterGlobalFunction("PolicyBuilder@ opcua()", asFUNCTION(factory_opcua), asCALL_CDECL);
    p_engine->RegisterGlobalFunction("PolicyBuilder@ eip()", asFUNCTION(factory_eip), asCALL_CDECL);
}

// ── PolicyEngine ──────────────────────────────────────────────────────────────

PolicyEngine::PolicyEngine()
    : active_table_(std::make_shared<RuleTable>()) {
}

sgrn::Result<void> PolicyEngine::loadScript(const std::string& t_path) {
    // Staging: compile + execute script into a fresh table.
    // On any failure, active_table_ stays unchanged (hot-reload safety).
    auto staging = std::make_shared<RuleTable>();

    sgrn::scripting::ScriptHost t_host;
    registerPolicyBindings(t_host, staging.get());

    if (auto t_r = t_host.loadFile(t_path); t_r.hasError())
        return t_r;

    asIScriptModule* p_mod = t_host.getEngine()->GetModule("main");
    if (!p_mod)
        return sgrn::Result<void>::Error("No compiled module");

    // Script must export void setup()
    asIScriptFunction* p_fn = p_mod->GetFunctionByDecl("void setup()");
    if (!p_fn)
        p_fn = p_mod->GetFunctionByDecl("void main()");
    if (!p_fn)
        return sgrn::Result<void>::Error("security.as must define 'void setup()' (or 'void main()')");

    asIScriptContext* p_ctx = t_host.getEngine()->CreateContext();
    p_ctx->Prepare(p_fn);
    int rc = p_ctx->Execute();
    if (rc == asEXECUTION_EXCEPTION) {
        std::string err = fmt::format("[policy] Exception in setup(): {}", p_ctx->GetExceptionString());
        p_ctx->Release();
        return sgrn::Result<void>::Error(err);
    }
    p_ctx->Release();

    // Atomic swap — readers see either old or new table, never a partial state
    active_table_.store(std::static_pointer_cast<const RuleTable>(staging));

    fmt::print("[policy] Loaded {} rules from '{}'\n", staging->size(), t_path);
    return {};
}

bool PolicyEngine::authorize(const RequestContext& t_ctx) const {
    auto tbl = active_table_.load();
    return tbl ? tbl->authorize(t_ctx) : false;
}

std::size_t PolicyEngine::ruleCount() const {
    auto tbl = active_table_.load();
    return tbl ? tbl->size() : 0;
}

std::string PolicyEngine::policyToJson() const {
    auto tbl = active_table_.load();
    if (!tbl)
        return "{\"rules\":[]}";
    return tbl->toJson();
}

// Helper: convert a CidrMatcher back to "x.x.x.x/n" string.
static std::string cidrToString(const CidrMatcher& t_c) {
    // Count prefix length from mask
    int bits = 0;
    uint32_t m = t_c.mask;
    while (m) {
        bits += (m & 1);
        m >>= 1;
    }
    struct in_addr t_a{};
    t_a.s_addr = htonl(t_c.network);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &t_a, buf, sizeof(buf));
    if (bits == 32)
        return buf; // bare IP
    return fmt::format("{}/{}", buf, bits);
}

static const char* protocolName(Protocol t_p) {
    switch (t_p) {
        case Protocol::S7:
            return "S7";
        case Protocol::HTTP:
            return "HTTP";
        case Protocol::WebSocket:
            return "WebSocket";
        case Protocol::Modbus:
            return "Modbus";
        case Protocol::OpcUA:
            return "OPC-UA";
        case Protocol::EthernetIP:
            return "EtherNet/IP";
    }
    return "Unknown";
}

std::string RuleTable::toJson() const {
    std::string out;
    out.reserve(512);
    out += "{\"rules\":[";
    bool first_rule = true;

    for (const auto& bucket : rules_by_protocol_) {
        for (const auto& t_r : bucket) {
            if (!first_rule)
                out += ',';
            first_rule = false;
            out += '{';
            out += fmt::format("\"protocol\":\"{}\",", protocolName(t_r.protocol));
            out += fmt::format("\"action\":\"{}\",", t_r.allow ? "ALLOW" : "DENY");
            out += fmt::format("\"specificity\":{},", t_r.specificity);

            // CIDRs
            out += "\"cidrs\":[";
            bool first = true;
            for (const auto& t_c : t_r.allow_cidrs) {
                if (!first)
                    out += ',';
                first = false;
                out += '"';
                out += cidrToString(t_c);
                out += '"';
            }
            out += "],";

            // DBs
            out += "\"dbs\":[";
            first = true;
            for (auto db : t_r.allow_db_numbers) {
                if (!first)
                    out += ',';
                first = false;
                out += std::to_string(db);
            }
            out += "],";

            out += fmt::format("\"any_db\":{},", t_r.any_db ? "true" : "false");

            // Origins
            out += "\"origins\":[";
            first = true;
            for (const auto& o : t_r.allow_origins) {
                if (!first)
                    out += ',';
                first = false;
                out += '"';
                out += o;
                out += '"';
            }
            out += "],";

            // Required headers
            out += "\"headers\":[";
            first = true;
            for (const auto& h : t_r.require_headers) {
                if (!first)
                    out += ',';
                first = false;
                out += '"';
                out += h;
                out += '"';
            }
            out += "],";

            // Session names (OPC-UA)
            out += "\"sessions\":[";
            first = true;
            for (const auto& s : t_r.allow_session_names) {
                if (!first)
                    out += ',';
                first = false;
                out += '"';
                out += s;
                out += '"';
            }
            out += "],";

            // Field Rules
            out += "\"fields\":[";
            first = true;
            for (const auto& f : t_r.field_rules) {
                if (!first)
                    out += ',';
                first = false;
                out += fmt::format("{{\"pattern\":\"{}\",\"read\":{},\"write\":{}}}", f.field_pattern, f.allow_read ? "true" : "false",
                    f.allow_write ? "true" : "false");
            }
            out += "]";

            out += '}';
        }
    }
    out += "]";
    out += fmt::format(",\"total\":{}", this->size());
    out += '}';
    return out;
}

} // namespace sgrn::gateway::security
