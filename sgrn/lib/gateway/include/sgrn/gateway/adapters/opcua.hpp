#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/opcua/tag_access.hpp>
#include <cstddef>
#include <memory>
#include <string>

// Forward declare — no open62541 types in public header
struct UA_Server;

namespace sgrn::gateway::twin
{
class PlcMemory;
} // namespace sgrn::gateway::twin

namespace sgrn::gateway
{
class SecurityManager;
} // namespace sgrn::gateway

namespace sgrn::scl
{
class PlcSchemaStore;
} // namespace sgrn::scl

namespace sgrn::gateway::adapters
{

class OpcUaAdapter {
public:
    OpcUaAdapter();
    ~OpcUaAdapter();

    OpcUaAdapter(const OpcUaAdapter&) = delete;
    OpcUaAdapter& operator=(const OpcUaAdapter&) = delete;
    OpcUaAdapter(OpcUaAdapter&&) = delete;
    OpcUaAdapter& operator=(OpcUaAdapter&&) = delete;

    sgrn::Result<void> start(const std::string& t_ip, uint16_t t_port, const ::sgrn::scl::PlcSchemaStore& t_registry,
        ::sgrn::gateway::twin::PlcMemory& t_s7_server, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security_manager);
    void stop();

    /// Discrete (TIA-style) tag projection: serves a "Tags" folder with one
    /// live variable node per tag (scalars native, UDTs as JSON strings).
    /// Backing lives wherever a PlcRuntime exists. Must be called before
    /// start(); null (default) keeps the DB-only address space.
    void setTagAccess(TagAccess t_access) {
        tag_access_ = std::move(t_access);
    }

    /// Number of currently active OPC-UA sessions (connected clients).
    std::size_t clientsCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TagAccess tag_access_;
};

} // namespace sgrn::gateway::adapters
