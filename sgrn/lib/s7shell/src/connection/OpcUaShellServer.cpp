#include "sgrn/s7shell/connection/OpcUaShellServer.hpp"

#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/s7shell/bindings/tag_opcua.hpp>

#include <fmt/core.h>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::shell
{

ScriptOpcUaServer::ScriptOpcUaServer(::sgrn::plcsim::runtime::PlcRuntimeSPtr tsp_rt, uint16_t t_port)
    : runtime_(std::move(tsp_rt))
    , port_(t_port) {
    adapter_ = std::make_unique<::sgrn::gateway::adapters::OpcUaAdapter>();
}

ScriptOpcUaServer::~ScriptOpcUaServer() {
    stopServer();
}

::sgrn::plcsim::runtime::PlcRuntimeSPtr ScriptOpcUaServer::getRuntime() const {
    return runtime_;
}

sgrn::Result<void, std::string> ScriptOpcUaServer::startServer() {
    if (!runtime_)
        return "OpcUaServer: PlcRuntime handle must be non-null";

    // Reuse the gateway's default Relaxed security policy for the virtual
    // simulation surface. Production gateways load a security.as policy.
    auto sp_security = std::make_shared<::sgrn::gateway::SecurityManager>();

    // Discrete tags: a live "Tags" folder (scalars native, UDTs as JSON).
    ::sgrn::s7shell::bindings::hookOpcuaTags(adapter_.get(), runtime_);

    auto res = adapter_->start("", port_, runtime_->getSchema(), runtime_->getMemory(), sp_security);
    if (res.hasError())
        return res.error();

    running_.store(true, std::memory_order_relaxed);
    fmt::print("[OpcUaServer] listening on opc.tcp://localhost:{}\n", port_);
    return {};
}

void ScriptOpcUaServer::stopServer() {
    running_.store(false, std::memory_order_relaxed);
    if (adapter_)
        adapter_->stop();
}

bool ScriptOpcUaServer::isRunning() const noexcept {
    return running_.load(std::memory_order_relaxed);
}

int ScriptOpcUaServer::clientsCount() const noexcept {
    return adapter_ ? static_cast<int>(adapter_->clientsCount()) : 0;
}

} // namespace sgrn::s7shell::shell
