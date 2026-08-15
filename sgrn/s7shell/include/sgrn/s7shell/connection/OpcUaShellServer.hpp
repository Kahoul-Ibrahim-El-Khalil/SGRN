#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/opcua.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sgrn::s7shell::shell
{

/// AngelScript-visible OPC UA server that exposes a PlcRuntime's virtual PLC
/// memory (schema + PlcMemory) as an OPC UA address space.
///
/// OPC UA analogue of ScriptS7Server: it reuses the gateway's OpcUaAdapter to
/// build the per-DB / per-field node tree from the loaded schema and routes
/// open62541 write callbacks into PlcMemory's command queue (flushed via
/// PlcMemory::processor()->processCommands()). A UA client such as UaExpert
/// can therefore write setpoints that immediately appear in the runtime the
/// rest of s7shell sees (e.g. rt.getJson(db)).
class ScriptOpcUaServer {
public:
    ScriptOpcUaServer(runtime::PlcRuntimeSPtr tsp_rt, uint16_t t_port);
    ~ScriptOpcUaServer();

    ScriptOpcUaServer(const ScriptOpcUaServer&) = delete;
    ScriptOpcUaServer& operator=(const ScriptOpcUaServer&) = delete;

    void addRef();
    void release();

    runtime::PlcRuntimeSPtr getRuntime() const;

    sgrn::Result<void, std::string> startServer();
    void stopServer();

    bool isRunning() const noexcept;
    int clientsCount() const noexcept;
    uint16_t getPort() const noexcept {
        return port_;
    }

private:
    int ref_count_{1};
    runtime::PlcRuntimeSPtr runtime_;
    uint16_t port_{0};
    std::unique_ptr<::sgrn::gateway::adapters::OpcUaAdapter> adapter_;
    std::atomic<bool> running_{false};
};

} // namespace sgrn::s7shell::shell
