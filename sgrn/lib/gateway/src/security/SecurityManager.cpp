#include <sgrn/gateway/security/SecurityManager.hpp>

#include <fmt/core.h>

namespace sgrn::gateway
{

// ── Scripted policy engine ────────────────────────────────────────────────────

sgrn::Result<void> SecurityManager::loadPolicyScript(const std::string& t_path) {
    auto engine = std::make_shared<security::PolicyEngine>();
    if (auto r = engine->loadScript(t_path); r.hasError())
        return r;

    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        policy_engine_.store(engine);
        policy_script_path_ = t_path;
    }
    fmt::print("[SecurityManager] Policy engine active (lock-free={}): {} rules from '{}'\n",
        policy_engine_.is_lock_free() ? "true" : "false", engine->ruleCount(), t_path);
    return {};
}

sgrn::Result<void> SecurityManager::reloadPolicy() {
    std::string t_path;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        t_path = policy_script_path_;
    }
    if (t_path.empty())
        return sgrn::Result<void>::Error("No policy script previously loaded");
    return loadPolicyScript(t_path);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string clientIpFromId(int t_sender_id, const std::unordered_map<int, std::string>& t_reg) {
    auto it = t_reg.find(t_sender_id);
    return it != t_reg.end() ? it->second : "";
}

// ── Authorization ─────────────────────────────────────────────────────────────

bool SecurityManager::authorizeWrite(int t_sender_id, int t_area, uint16_t t_db_number) const {
    // Area protection: process inputs/outputs are always read-only for PLC clients.
    if (t_area == static_cast<int>(S7Area::ProcessInputs) || t_area == static_cast<int>(S7Area::ProcessOutputs))
        return false;

    auto engine = policy_engine_.load();
    std::string t_client_ip;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        t_client_ip = clientIpFromId(t_sender_id, client_registry_);
    }

    // --- Script-driven path ---
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = security::Protocol::S7;
        ctx.client_ip = t_client_ip;
        ctx.db_number = t_db_number;
        return engine->authorize(ctx);
    }

    // --- Legacy ACL fallback ---
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }

    if (active_policy == ::sgrn::scl::SecurityPolicy::Relaxed)
        return true;

    if (t_client_ip.empty())
        return false; // unknown client denied in Strict mode

    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        auto it = db_acl_.find(t_db_number);
        if (it != db_acl_.end())
            return it->second == t_client_ip;
    }

    return false; // no ACL entry → deny
}

bool SecurityManager::authorizeHttp(const std::string& t_client_ip, const std::string& t_origin,
    const std::vector<std::string>& t_header_names, std::optional<uint16_t> t_db_number) const {
    auto engine = policy_engine_.load();
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = security::Protocol::HTTP;
        ctx.client_ip = t_client_ip;
        ctx.db_number = t_db_number;
        ctx.http_origin = t_origin;
        ctx.present_headers = t_header_names;
        return engine->authorize(ctx);
    }
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }
    return active_policy == ::sgrn::scl::SecurityPolicy::Relaxed;
}

bool SecurityManager::authorizeWebSocket(
    const std::string& t_client_ip, const std::string& t_origin, std::optional<uint16_t> t_db_number) const {
    auto engine = policy_engine_.load();
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = security::Protocol::WebSocket;
        ctx.client_ip = t_client_ip;
        ctx.db_number = t_db_number;
        ctx.http_origin = t_origin;
        return engine->authorize(ctx);
    }
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }
    return active_policy == ::sgrn::scl::SecurityPolicy::Relaxed;
}

bool SecurityManager::authorizeModbus(const std::string& t_client_ip, std::optional<uint16_t> t_db_number) const {
    auto engine = policy_engine_.load();
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = security::Protocol::Modbus;
        ctx.client_ip = t_client_ip;
        ctx.db_number = t_db_number;
        return engine->authorize(ctx);
    }
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }
    return active_policy == ::sgrn::scl::SecurityPolicy::Relaxed;
}

bool SecurityManager::authorizeOpcUa(
    const std::string& t_client_ip, const std::string& t_session_name, std::optional<uint16_t> t_db_number) const {
    auto engine = policy_engine_.load();
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = security::Protocol::OpcUA;
        ctx.client_ip = t_client_ip;
        ctx.db_number = t_db_number;
        ctx.opc_session_name = t_session_name;
        return engine->authorize(ctx);
    }
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }
    return active_policy == ::sgrn::scl::SecurityPolicy::Relaxed;
}

bool SecurityManager::authorizeEip(const std::string& t_client_ip) const {
    auto engine = policy_engine_.load();
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = security::Protocol::EthernetIP;
        ctx.client_ip = t_client_ip;
        return engine->authorize(ctx);
    }
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }
    return active_policy == ::sgrn::scl::SecurityPolicy::Relaxed;
}

bool SecurityManager::authorizeField(security::Protocol t_protocol, const std::string& t_client_ip, std::optional<uint16_t> t_db_number,
    const std::string& t_field_path, bool t_is_write, const std::string& t_http_origin, const std::vector<std::string>& t_present_headers,
    const std::string& t_opc_session_name) const {
    auto engine = policy_engine_.load();
    if (engine) {
        security::RequestContext ctx;
        ctx.protocol = t_protocol;
        ctx.client_ip = t_client_ip;
        ctx.db_number = t_db_number;
        ctx.field_path = t_field_path;
        ctx.is_write = t_is_write;
        ctx.http_origin = t_http_origin;
        ctx.present_headers = t_present_headers;
        ctx.opc_session_name = t_opc_session_name;
        return engine->authorize(ctx);
    }
    ::sgrn::scl::SecurityPolicy active_policy;
    {
        std::lock_guard<std::mutex> lock(legacy_mutex_);
        active_policy = policy_;
    }
    return active_policy == ::sgrn::scl::SecurityPolicy::Relaxed;
}

// ── Legacy ACL ────────────────────────────────────────────────────────────────

void SecurityManager::registerClient(int t_sender_id, const std::string& t_ip) {
    std::lock_guard<std::mutex> lock(legacy_mutex_);
    client_registry_[t_sender_id] = t_ip;
}

void SecurityManager::setPolicy(::sgrn::scl::SecurityPolicy t_policy) {
    std::lock_guard<std::mutex> lock(legacy_mutex_);
    policy_ = t_policy;
}

void SecurityManager::setAllowedIp(uint16_t t_db_number, const std::string& t_ip) {
    std::lock_guard<std::mutex> lock(legacy_mutex_);
    db_acl_[t_db_number] = t_ip;
}

void SecurityManager::clear() {
    std::lock_guard<std::mutex> lock(legacy_mutex_);
    db_acl_.clear();
    client_registry_.clear();
    policy_engine_.store(nullptr);
    policy_script_path_.clear();
}

std::string SecurityManager::policyToJson() const {
    auto engine = policy_engine_.load();
    if (engine)
        return engine->policyToJson();

    std::lock_guard<std::mutex> lock(legacy_mutex_);
    // Legacy ACL fallback: emit a synthetic JSON representation
    std::string out;
    out.reserve(256);
    out += "{\"rules\":[";
    bool first = true;
    for (const auto& [db, t_ip] : db_acl_) {
        if (!first)
            out += ',';
        first = false;
        out += fmt::format("{{\"protocol\":\"S7\",\"action\":\"ALLOW\","
                           "\"specificity\":32,\"cidrs\":[\"{}\"],"
                           "\"dbs\":[{}],\"any_db\":false,"
                           "\"origins\":[],\"headers\":[],\"sessions\":[]}}",
            t_ip, db);
    }
    out += fmt::format("],\"total\":{}}}", db_acl_.size());
    return out;
}

} // namespace sgrn::gateway
