#pragma once
// =============================================================================
// PolicyEngine.hpp — Compiled security rule table for the SGRN gateway
//
// Design contract:
//   * "Build-once, evaluate-cheap": security.as is compiled at boot/reload.
//     The AS callbacks populate a C++ RuleTable. Per-request authorization is
//     a pure read-only O(n_rules_for_protocol) scan — wait-free after boot.
//   * Deny-by-default: if no rule matches, access is DENIED.
//   * Precedence: most-specific CIDR wins; rule order in .as file is tie-break.
//   * Protocol-specific builders (S7Rules, HttpRules, etc.) present a typed API
//     to AngelScript — each exposes only the predicates valid for that protocol.
// =============================================================================
#include <sgrn/scripting/ScriptHost.hpp>

#include <sgrn/utils/network.hpp>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class CScriptArray;

namespace sgrn::gateway::security
{

enum class Protocol : uint8_t {
    S7 = 0,
    HTTP = 1,
    WebSocket = 2,
    Modbus = 3,
    OpcUA = 4,
    EthernetIP = 5,
};

using CidrMatcher = ::sgrn::utils::network::CidrMatcher;
using ::sgrn::utils::network::parseCidr;
using ::sgrn::utils::network::parseIp;
using ::sgrn::utils::network::parseIpv4OrResolve;
using ::sgrn::utils::network::parseIpv4Strict;

// ── A single authorization rule ───────────────────────────────────────────────

struct Rule {
    Protocol protocol;

    // --- IP / CIDR predicate (all protocols) ---
    std::vector<CidrMatcher> allow_cidrs; // OR: any match → pass this predicate

    // --- S7 / Modbus / EthernetIP (layer-3-only) ---
    std::vector<uint16_t> allow_db_numbers; // empty = any DB
    bool any_db{true};                      // true when allow_db_numbers is empty

    // --- HTTP / WebSocket ---
    std::vector<std::string> allow_origins;   // empty = any origin
    std::vector<std::string> require_headers; // AND: all must be present (name only)

    // --- OPC-UA ---
    std::vector<std::string> allow_session_names; // empty = any session

    // --- Field-level (Part 2) ---
    struct FieldRule {
        std::string field_pattern;
        bool allow_read{true};
        bool allow_write{true};
    };
    std::vector<FieldRule> field_rules;

    // --- Action ---
    bool allow{true}; // true = ALLOW, false = DENY

    // --- Specificity for tie-break (computed by the engine) ---
    int specificity{0};
};

// ── Runtime context passed to authorizeXxx() ─────────────────────────────────

struct RequestContext {
    Protocol protocol;
    std::string_view client_ip;                   // dotted-decimal
    std::optional<uint16_t> db_number;            // S7/Modbus/HTTP DB or nullopt
    std::string_view http_origin;                 // HTTP/WS Origin header
    std::span<const std::string> present_headers; // header names present in request (or view-based span)
    std::string_view opc_session_name;            // OPC-UA session name
    std::string_view field_path;                  // empty for whole-DB ops
    bool is_write{false};                         // read vs write

    // LIFETIME CONTRACT: RequestContext is a transient view structure and MUST NOT
    // outlive the synchronous authorize() call stack frame.
};

// ── Rule table: filled by the AS policy script, queried per-request ───────────

class RuleTable {
public:
    void addRule(Rule t_r);
    void clear();

    /// Returns true if request is authorized. Deny-by-default.
    bool authorize(const RequestContext& t_ctx) const;

    std::size_t size() const {
        std::size_t total = 0;
        for (const auto& bucket : rules_by_protocol_) {
            total += bucket.size();
        }
        return total;
    }

    /// Serialize the rule table to a JSON string.
    std::string toJson() const;

    void markProtocolConfigured(Protocol t_p) {
        configured_protocols_.insert(t_p);
    }

private:
    static constexpr size_t kProtocolCount = 6;
    std::array<std::vector<Rule>, kProtocolCount> rules_by_protocol_;
    std::set<Protocol> configured_protocols_;
};

// ── AngelScript builder types (exposed to .as scripts) ───────────────────────
// These are value types in AS; each builder call appends to the RuleTable.

class PolicyBuilder {
public:
    explicit PolicyBuilder(RuleTable* tp_table, Protocol t_proto)
        : table_(tp_table)
        , protocol_(t_proto) {
        pending_.protocol = t_proto;
    }

    // Fluent API registered to AS
    PolicyBuilder* allowIp(const std::string& t_ip);
    PolicyBuilder* allowCidr(const std::string& t_cidr);
    PolicyBuilder* allowDb(int t_db_number);
    PolicyBuilder* allowDbArray(CScriptArray* tp_dbs);
    PolicyBuilder* allowOrigin(const std::string& t_origin);
    PolicyBuilder* requireHeader(const std::string& t_header_name);
    PolicyBuilder* allowSession(const std::string& t_session_name);
    PolicyBuilder* allowField(const std::string& t_pattern);
    PolicyBuilder* allowFieldArray(CScriptArray* tp_patterns);
    PolicyBuilder* allowFieldRead(const std::string& t_pattern);
    PolicyBuilder* allowFieldReadArray(CScriptArray* tp_patterns);
    PolicyBuilder* denyFieldWrite(const std::string& t_pattern);
    PolicyBuilder* denyFieldWriteArray(CScriptArray* tp_patterns);
    void commit(bool t_allow_action = true);

    // ref-counting for AS
    void addRef() {
        ++ref_;
    }
    void release() {
        if (--ref_ == 0)
            delete this;
    }

private:
    RuleTable* table_;
    Protocol protocol_;
    Rule pending_;
    std::atomic<int> ref_{1};
};

// ── Policy engine: owns the ScriptHost and RuleTable ─────────────────────────

class PolicyEngine {
public:
    PolicyEngine();
    ~PolicyEngine() = default;

    PolicyEngine(const PolicyEngine&) = delete;
    PolicyEngine& operator=(const PolicyEngine&) = delete;

    /// Load (or reload) a security.as file.
    /// Thread-safe: replaces table atomically via swap.
    sgrn::Result<void> loadScript(const std::string& t_path);

    /// Authorize a request. Wait-free read path.
    bool authorize(const RequestContext& t_ctx) const;

    std::size_t ruleCount() const;

    /// Serialize the active rule table to a JSON string.
    std::string policyToJson() const;

private:
    std::atomic<std::shared_ptr<const RuleTable>> active_table_;
};

/// Register all PolicyEngine AS types into an existing ScriptHost.
void registerPolicyBindings(sgrn::scripting::ScriptHost& t_host, RuleTable* tp_staging_table);

} // namespace sgrn::gateway::security
