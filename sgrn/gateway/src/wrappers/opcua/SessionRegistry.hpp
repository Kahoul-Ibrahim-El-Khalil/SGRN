#pragma once

#include <open62541/types_generated.h> // UA_StatusCode, UA_NodeId, UA_ExtensionObject, ...

#include <cstddef>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// UA_Server is an opaque incomplete type in open62541 — safe to forward-declare.
struct UA_Server;
// UA_AccessControl is an opaque plugin struct — only referenced by pointer below.
struct UA_AccessControl;

/// Signatures of the open62541 access-control hooks we wrap (and chain to).
using ActivateSessionFn = UA_StatusCode (*)(UA_Server*, UA_AccessControl*, const UA_EndpointDescription*, const UA_ByteString*,
    const UA_NodeId*, const UA_ExtensionObject*, void**);
using CloseSessionFn = void (*)(UA_Server*, UA_AccessControl*, const UA_NodeId*, void*);

namespace sgrn::gateway::wrappers::opcua
{

struct SessionRecord {
    std::string ip;
    std::string id;
};

/// Thread-safe registry of active OPC-UA sessions, keyed by session id string.
///
/// installAccessControl() must be called after UA_ServerConfig_setMinimal but
/// before UA_Server_run_startup. It chains to any pre-existing access-control
/// hooks (so we never clobber an embedding application's auth) while tracking
/// session activation/closure so clientsCount() and resolveIp() stay accurate.
class SessionRegistry {
public:
    void onActivate(const std::string& t_session_id_str, const std::string& t_remote_addr);
    void onClose(const std::string& t_session_id_str);

    /// Returns the remote IP for a session, or "" if not found / not tracked.
    std::string resolveIp(const std::string& t_session_id_str) const;

    /// Number of currently active OPC-UA sessions (connected clients).
    std::size_t clientsCount() const;

    /// Install custom accessControl hooks into the server config.
    void installAccessControl(UA_Server* tp_server);

private:
    static UA_StatusCode onActivateSession(UA_Server* tp_server, UA_AccessControl* tp_ac, const UA_EndpointDescription* t_endpoint,
        const UA_ByteString* t_cert, const UA_NodeId* t_session_id, const UA_ExtensionObject* t_token, void** t_session_context);
    static void onCloseSession(UA_Server* tp_server, UA_AccessControl* tp_ac, const UA_NodeId* t_session_id, void* t_session_context);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SessionRecord> sessions_;

    // Original hooks captured for chaining (NULL for the default anonymous
    // minimal config). Typed aliases keep call sites cast-free.
    ActivateSessionFn original_activate_session_{nullptr};
    CloseSessionFn original_close_session_{nullptr};
    void* original_context_{nullptr};
};

} // namespace sgrn::gateway::wrappers::opcua
