#pragma once

#include <sgrn/gateway/security/PolicyEngine.hpp>
#include <sgrn/scl/types.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace sgrn::gateway
{

enum class MemoryArea : int {
    ProcessInputs = 0x81,
    ProcessOutputs = 0x82,
    Merkers = 0x83,
    DataBlocks = 0x84,
    Counters = 0x1C,
    Timers = 0x1D,
};

/// Backward compat alias
using S7Area = MemoryArea;

/// SecurityManager
///
/// Supports two authorization modes:
///   1. Script-driven (preferred): load a security.as policy file.
///      Rules are evaluated by PolicyEngine. Hot-reload via reloadPolicy().
///   2. Legacy ACL map (backward compat): setAllowedIp() / setPolicy().
///      Active when no policy script is loaded.
class SecurityManager {
public:
    SecurityManager() = default;

    // ── Scripted policy engine ──────────────────────────────────────────────

    /// Load (or hot-reload) a security.as policy script.
    /// On success, the script engine takes over all authorization decisions.
    /// On failure, the previous rule table (or legacy ACL) remains active.
    sgrn::Result<void> loadPolicyScript(const std::string& t_path);

    /// Reload the last successfully loaded script (for SIGHUP handlers).
    sgrn::Result<void> reloadPolicy();

    // ── Request authorization ──────────────────────────────────────────────

    /// Authorize an S7 write from a registered client.
    bool authorizeWrite(int t_sender_id, int t_area, uint16_t t_db_number) const;

    /// Authorize an HTTP/REST request.
    bool authorizeHttp(const std::string& t_client_ip, const std::string& t_origin, const std::vector<std::string>& t_header_names,
        std::optional<uint16_t> t_db_number = std::nullopt) const;

    /// Authorize a WebSocket connection or subscribe.
    bool authorizeWebSocket(
        const std::string& t_client_ip, const std::string& t_origin, std::optional<uint16_t> t_db_number = std::nullopt) const;

    /// Authorize a Modbus TCP read/write.
    bool authorizeModbus(const std::string& t_client_ip, std::optional<uint16_t> t_db_number = std::nullopt) const;

    /// Authorize an OPC-UA write.
    bool authorizeOpcUa(
        const std::string& t_client_ip, const std::string& t_session_name = {}, std::optional<uint16_t> t_db_number = std::nullopt) const;

    /// Authorize an EtherNet/IP request.
    bool authorizeEip(const std::string& t_client_ip) const;

    /// Authorize a field read or write for a specific protocol.
    bool authorizeField(security::Protocol t_protocol, const std::string& t_client_ip, std::optional<uint16_t> t_db_number,
        const std::string& t_field_path, bool t_is_write, const std::string& t_http_origin = "",
        const std::vector<std::string>& t_present_headers = {}, const std::string& t_opc_session_name = "") const;

    // ── Legacy ACL (backward compat when no script is loaded) ─────────────

    void registerClient(int t_sender_id, const std::string& t_ip);
    void setPolicy(::sgrn::scl::SecurityPolicy t_policy);
    void setAllowedIp(uint16_t t_db_number, const std::string& t_ip);
    void clear();

    /// Serialize the active security policy to JSON for the web UI.
    std::string policyToJson() const;

private:
    bool scriptLoaded() const {
        return policy_engine_.load() != nullptr;
    }

    // --- Scripted path ---
    std::atomic<std::shared_ptr<security::PolicyEngine>> policy_engine_;
    std::string policy_script_path_;

    // --- Legacy path ---
    ::sgrn::scl::SecurityPolicy policy_{::sgrn::scl::SecurityPolicy::Relaxed};
    std::unordered_map<uint16_t, std::string> db_acl_;
    std::unordered_map<int, std::string> client_registry_;
    mutable std::mutex legacy_mutex_;
};

using SecurityManagerSptr = std::shared_ptr<SecurityManager>;
} // namespace sgrn::gateway
