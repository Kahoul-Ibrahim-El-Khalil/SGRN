#include <sgrn/gateway/wrappers/opcua/Server.hpp>

#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>

#include <open62541/server_config_default.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

#include <utility>

namespace sgrn::gateway::wrappers::opcua
{

struct Server::LogFilter {
    static void log(void* tp_ctx, UA_LogLevel t_level, UA_LogCategory t_cat, const char* tp_msg, va_list t_args) {
        if (t_level < UA_LOGLEVEL_WARNING)
            return;
        auto* p_orig = static_cast<UA_Logger*>(tp_ctx);
        if (p_orig->log)
            p_orig->log(p_orig->context, t_level, t_cat, tp_msg, t_args);
    }
};

Server::Server(uint16_t t_port)
    : port_(t_port) {
}

Server::~Server() noexcept {
    destroy();
}

Server::Server(Server&& t_other) noexcept
    : port_(t_other.port_)
    , server_(std::exchange(t_other.server_, nullptr))
    , orig_logger_(t_other.orig_logger_) {
}

Server& Server::operator=(Server&& t_other) noexcept {
    if (this != &t_other) {
        destroy();
        port_ = t_other.port_;
        server_ = std::exchange(t_other.server_, nullptr);
        orig_logger_ = t_other.orig_logger_;
    }
    return *this;
}

void Server::ensureCreated() {
    if (server_)
        return;
    server_ = UA_Server_new();
    if (!server_)
        return;
    UA_ServerConfig* p_config = UA_Server_getConfig(server_);
    UA_ServerConfig_setMinimal(p_config, port_, nullptr);
}

void Server::suppressInfoLogs() {
    ensureCreated();
    if (!server_)
        return;
    UA_ServerConfig* p_config = UA_Server_getConfig(server_);
    orig_logger_ = *p_config->logging;
    p_config->logging->log = &LogFilter::log;
    p_config->logging->context = &orig_logger_;
    p_config->logging->clear = nullptr;
}

void Server::installTypeRegistry(TypeRegistry& t_registry) {
    ensureCreated();
    if (!server_)
        return;
    t_registry.attachTo(UA_Server_getConfig(server_));
}

void Server::destroy() noexcept {
    if (server_) {
        UA_Server_delete(server_);
        server_ = nullptr;
    }
}

sgrn::Result<NodeId> Server::addObjectNode(const NodeId& t_parent, std::string_view t_browse_name, std::string_view t_node_id_str) {
    if (!server_)
        return "addObjectNode: server not initialised";

    UA_NodeId out_id = UA_NODEID_NULL;
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en"), const_cast<char*>(t_browse_name.data()));

    UA_NodeId requested = UA_NODEID_STRING_ALLOC(1, t_node_id_str.data());
    UA_QualifiedName qn = UA_QUALIFIEDNAME_ALLOC(1, t_browse_name.data());

    UA_StatusCode sc = UA_Server_addObjectNode(server_, requested, t_parent.get(), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn,
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, nullptr, &out_id);

    UA_NodeId_clear(&requested);
    UA_QualifiedName_clear(&qn);

    if (sc != UA_STATUSCODE_GOOD)
        return fmt::format("addObjectNode({}: {}", t_node_id_str, UA_StatusCode_name(sc));

    NodeId result = nodeIdFromRaw(out_id);
    UA_NodeId_clear(&out_id);
    return result;
}

sgrn::Result<NodeId> Server::addVariableNode(
    const NodeId& t_parent, std::string_view t_browse_name, std::string_view t_node_id_str, const VariableAttributes& t_attrs) {
    if (!server_)
        return "addVariableNode: server not initialised";

    UA_NodeId out_id = UA_NODEID_NULL;
    UA_VariableAttributes ua_attrs = t_attrs.build();

    UA_NodeId requested = UA_NODEID_STRING_ALLOC(1, t_node_id_str.data());
    UA_QualifiedName qn = UA_QUALIFIEDNAME_ALLOC(1, t_browse_name.data());

    UA_StatusCode sc = UA_Server_addVariableNode(server_, requested, t_parent.get(), UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), qn,
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), ua_attrs, nullptr, &out_id);

    UA_NodeId_clear(&requested);
    UA_QualifiedName_clear(&qn);

    if (sc != UA_STATUSCODE_GOOD)
        return fmt::format("addVariableNode({}: {}", t_node_id_str, UA_StatusCode_name(sc));

    NodeId result = nodeIdFromRaw(out_id);
    UA_NodeId_clear(&out_id);
    return result;
}

sgrn::Result<void> Server::writeDataValue(const NodeId& t_node, const DataValue& t_value) {
    if (!server_)
        return "writeDataValue: server not initialised";

    UA_StatusCode sc = UA_Server_writeDataValue(server_, t_node.get(), t_value.get());
    if (sc != UA_STATUSCODE_GOOD)
        return fmt::format("writeDataValue: {}", UA_StatusCode_name(sc));
    return {};
}

} // namespace sgrn::gateway::wrappers::opcua
