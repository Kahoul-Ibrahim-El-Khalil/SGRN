#include "sgrn/s7shell/connection/OpcUaShellServer.hpp"

#include <sgrn/gateway/security/SecurityManager.hpp>

#include <fmt/core.h>

namespace sgrn::s7shell::shell
{

ScriptOpcUaServer::ScriptOpcUaServer(runtime::PlcRuntimeSPtr tsp_rt, uint16_t t_port)
    : runtime_(std::move(tsp_rt))
    , port_(t_port) {
    adapter_ = std::make_unique<::sgrn::gateway::adapters::OpcUaAdapter>();
}

ScriptOpcUaServer::~ScriptOpcUaServer() {
    stopServer();
}

void ScriptOpcUaServer::addRef() {
    ++ref_count_;
}

void ScriptOpcUaServer::release() {
    if (--ref_count_ == 0)
        delete this;
}

runtime::PlcRuntimeSPtr ScriptOpcUaServer::getRuntime() const {
    return runtime_;
}

sgrn::Result<void, std::string> ScriptOpcUaServer::startServer() {
    if (!runtime_)
        return "OpcUaServer: PlcRuntime handle must be non-null";

    // Reuse the gateway's default Relaxed security policy for the virtual
    // simulation surface. Production gateways load a security.as policy.
    auto sp_security = std::make_shared<::sgrn::gateway::SecurityManager>();

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
