#pragma once

#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <open62541/server.h> // UA_Server, UA_AccessControl, UA_NodeId, ...

namespace sgrn::gateway::wrappers::opcua
{

struct SessionRecord {
    std::string ip;
    std::string t_session_id_str;
};

/// Thread-safe registry of active OPC-UA sessions, keyed by session id string.
///
/// installAccessControl() must be called after UA_ServerConfig_setMinimal but
/// before UA_Server_run_startup.
class SessionRegistry {
public:
    void onActivate(const std::string& t_session_id_str, const std::string& t_remote_addr);
    void onClose(const std::string& t_session_id_str);

    /// Returns the remote IP for a session, or "" if not found.
    std::string resolveIp(const std::string& t_session_id_str) const;

    /// Install custom accessControl hooks into the server config.
    ///
    /// Chains to any pre-existing activateSession / closeSession hooks so we
    /// never clobber an embedding application's auth, then records session
    /// open/close events in the registry.
    void installAccessControl(UA_Server* tp_server);

    /// Number of currently tracked sessions (live connections).
    std::size_t clientsCount() const;

private:
    // open62541 accessControl hook typedefs (generated header)
    using ActivateSessionFn = UA_StatusCode (*)(UA_Server*, UA_AccessControl*, const UA_EndpointDescription*, const UA_ByteString*,
        const UA_NodeId*, const UA_ExtensionObject*, void**);
    using CloseSessionFn = void (*)(UA_Server*, UA_AccessControl*, const UA_NodeId*, void*);

    static UA_StatusCode onActivateSession(UA_Server* tp_server, UA_AccessControl* tp_ac, const UA_EndpointDescription* t_endpoint,
        const UA_ByteString* t_cert, const UA_NodeId* t_session_id, const UA_ExtensionObject* t_token, void** t_session_context);
    static void onCloseSession(UA_Server* tp_server, UA_AccessControl* tp_ac, const UA_NodeId* t_session_id, void* t_session_context);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SessionRecord> sessions_;
    ActivateSessionFn original_activate_session_{nullptr};
    CloseSessionFn original_close_session_{nullptr};
};

} // namespace sgrn::gateway::wrappers::opcua
