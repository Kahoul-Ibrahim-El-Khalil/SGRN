#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/OpcUaShellServer.hpp>

#include <angelscript.h>

namespace sgrn::s7shell::bindings
{
using sgrn::Result;
using namespace sgrn::s7shell::shell;

static ScriptOpcUaServer* OpcUaServer_factory(PlcRuntimeWrapper* tp_rt, uint16_t t_port) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("OpcUaServer: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new ScriptOpcUaServer(tp_rt->getImpl(), t_port);
}

static bool OpcUaServer_start(ScriptOpcUaServer* tp_server) {
    if (!tp_server) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("OpcUaServer: null server handle");
        return false;
    }
    auto res = tp_server->startServer();
    if (res.hasError()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException(("OpcUaServer start failed: " + res.error()).c_str());
        return false;
    }
    return true;
}

static int OpcUaServer_clients(ScriptOpcUaServer* tp_server) {
    return tp_server ? tp_server->clientsCount() : 0;
}

Result<void, std::string> registerOpcUaServer(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "OpcUaServer");

    SGRN_AS_REFCOUNTED(tp_engine, "OpcUaServer", ScriptOpcUaServer);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "OpcUaServer", asBEHAVE_FACTORY, "OpcUaServer@ f(PlcRuntime@, uint16 = 4840)", asFUNCTION(OpcUaServer_factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("OpcUaServer", "bool start()", asFUNCTION(OpcUaServer_start), asCALL_CDECL_OBJLAST));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("OpcUaServer", "void stop()", asMETHOD(ScriptOpcUaServer, stopServer), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("OpcUaServer", "bool isRunning() const", asMETHOD(ScriptOpcUaServer, isRunning), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("OpcUaServer", "int clientsCount() const", asFUNCTION(OpcUaServer_clients), asCALL_CDECL_OBJLAST));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("OpcUaServer", "uint16 getPort() const", asMETHOD(ScriptOpcUaServer, getPort), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
