#include <sgrn/gateway/wrappers/opcua/SessionRegistry.hpp>

#include <open62541/plugin/accesscontrol.h>     // UA_AccessControl
#include <open62541/server.h>                   // UA_Server_getConfig
#include <open62541/server_config_default.h>    // UA_ServerConfig (generated)
#include <open62541/types.h>                    // UA_NodeId_print
#include <open62541/types_generated.h>          // UA_NodeId, UA_StatusCode, ...
#include <open62541/types_generated_handling.h> // UA_String_clear

#include <mutex>
#include <string>

namespace
{

// Stringify an open62541 session NodeId. The remote IP is not available inside
// activateSession (it lives on the secure channel), so we record a stable
// session id string and leave the IP empty — clientsCount() still reflects the
// live connection set.
std::string sessionIdToString(const UA_NodeId* t_session_id) {
    if (!t_session_id)
        return "";
    UA_String sid{};
    UA_NodeId_print(t_session_id, &sid);
    std::string out = sid.length ? std::string(reinterpret_cast<const char*>(sid.data), sid.length) : std::string();
    UA_String_clear(&sid);
    return out;
}

} // namespace

namespace sgrn::gateway::wrappers::opcua
{

void SessionRegistry::onActivate(const std::string& t_session_id_str, const std::string& t_remote_addr) {
    std::unique_lock lock(mutex_);
    sessions_[t_session_id_str] = SessionRecord{t_remote_addr, t_session_id_str};
}

void SessionRegistry::onClose(const std::string& t_session_id_str) {
    std::unique_lock lock(mutex_);
    sessions_.erase(t_session_id_str);
}

std::string SessionRegistry::resolveIp(const std::string& t_session_id_str) const {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(t_session_id_str);
    return it != sessions_.end() ? it->second.ip : "";
}

std::size_t SessionRegistry::clientsCount() const {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

void SessionRegistry::installAccessControl(UA_Server* tp_server) {
    if (!tp_server)
        return;
    UA_ServerConfig* p_config = UA_Server_getConfig(tp_server);
    if (!p_config)
        return;
    UA_AccessControl& ac = p_config->accessControl;

    // Capture whatever hooks were already present (NULL for the default
    // anonymous minimal config) so we can chain to them and never clobber an
    // embedding application's auth.
    original_activate_session_ = ac.activateSession;
    original_close_session_ = ac.closeSession;
    original_context_ = ac.context;

    ac.context = this;
    ac.activateSession = &SessionRegistry::onActivateSession;
    ac.closeSession = &SessionRegistry::onCloseSession;
    installed_ = true;
}

void SessionRegistry::uninstallAccessControl(UA_Server* tp_server) {
    if (!installed_)
        return;
    installed_ = false;
    if (!tp_server)
        return;
    UA_ServerConfig* p_config = UA_Server_getConfig(tp_server);
    if (!p_config)
        return;
    UA_AccessControl& ac = p_config->accessControl;

    // Restore the hooks captured at install time. In particular ac->context
    // must point at open62541's own AccessControlContext again: its default
    // clear callback frees the context, and freeing this registry with free()
    // aborts teardown. Call before UA_Server_delete, after the server thread
    // is joined (no session callbacks in flight).
    ac.activateSession = original_activate_session_;
    ac.closeSession = original_close_session_;
    ac.context = original_context_;
    original_activate_session_ = nullptr;
    original_close_session_ = nullptr;
    original_context_ = nullptr;
}

UA_StatusCode SessionRegistry::onActivateSession(UA_Server* tp_server, UA_AccessControl* tp_ac, const UA_EndpointDescription* t_endpoint,
    const UA_ByteString* t_cert, const UA_NodeId* t_session_id, const UA_ExtensionObject* t_token, void** t_session_context) {
    auto* self = tp_ac ? static_cast<SessionRegistry*>(tp_ac->context) : nullptr;
    UA_StatusCode sc = UA_STATUSCODE_GOOD;
    if (self && self->original_activate_session_) {
        void* saved_ctx = tp_ac->context;
        tp_ac->context = self->original_context_;
        sc = self->original_activate_session_(tp_server, tp_ac, t_endpoint, t_cert, t_session_id, t_token, t_session_context);
        tp_ac->context = saved_ctx;
    }
    if (sc == UA_STATUSCODE_GOOD && self && t_session_id)
        self->onActivate(sessionIdToString(t_session_id), ""); // remote IP not available pre-auth
    return sc;
}

void SessionRegistry::onCloseSession(
    UA_Server* tp_server, UA_AccessControl* tp_ac, const UA_NodeId* t_session_id, void* t_session_context) {
    auto* self = tp_ac ? static_cast<SessionRegistry*>(tp_ac->context) : nullptr;
    if (self && self->original_close_session_) {
        void* saved_ctx = tp_ac->context;
        tp_ac->context = self->original_context_;
        if (t_session_id)
            self->onClose(sessionIdToString(t_session_id));
        self->original_close_session_(tp_server, tp_ac, t_session_id, t_session_context);
        tp_ac->context = saved_ctx;
    } else if (self && t_session_id) {
        self->onClose(sessionIdToString(t_session_id));
    }
}

} // namespace sgrn::gateway::wrappers::opcua
