#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>
#include <sgrn/gateway/wrappers/opcua/VariableAttributes.hpp>

#include <open62541/server.h>

#include <string_view>

namespace sgrn::gateway::wrappers::opcua
{

class TypeRegistry;

/// RAII wrapper over UA_Server* for configuration and address-space helpers.
///
/// The adapter owns the server thread and startup sequence (run_startup,
/// repeated callbacks, iterate) — matching the legacy OpcUaAdapter on main.
/// This class only manages UA_Server lifetime and pre-start configuration.
class Server {
public:
    explicit Server(uint16_t t_port);
    ~Server() noexcept;

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void suppressInfoLogs();
    void installTypeRegistry(TypeRegistry& t_registry);

    /// Delete the UA_Server. The adapter must join its server thread first.
    void destroy() noexcept;

    sgrn::Result<NodeId> addObjectNode(const NodeId& t_parent, std::string_view t_browse_name, std::string_view t_node_id_str);

    sgrn::Result<NodeId> addVariableNode(
        const NodeId& t_parent, std::string_view t_browse_name, std::string_view t_node_id_str, const VariableAttributes& t_attrs);

    sgrn::Result<void> writeDataValue(const NodeId& t_node, const DataValue& t_value);

    UA_Server* raw() noexcept {
        return server_;
    }

private:
    struct LogFilter;

    void ensureCreated();

    uint16_t port_{0};
    UA_Server* server_{nullptr};
    UA_Logger orig_logger_{};
};

} // namespace sgrn::gateway::wrappers::opcua
