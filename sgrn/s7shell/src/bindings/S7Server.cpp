#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/S7ShellServer.hpp>
#include <angelscript.h>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell;
using sgrn::Result;
static ScriptS7Server* S7Server_factory(PlcRuntimeWrapper* tp_rt, const std::string& t_ip, uint16_t t_port) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("S7Server: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new ScriptS7Server(tp_rt->getImpl(), t_ip, t_port);
}

static bool S7Server_start(ScriptS7Server* tp_server) {
    auto res = tp_server->startServer();
    if (res.hasError()) {
        if (auto* p_ctx = asGetActiveContext()) {
            p_ctx->SetException(fmt::format("S7Server start failed: {}", toString(res.error())).c_str());
        }
        return false;
    }
    return true;
}

Result<void, std::string> registerS7Server(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "S7Server");

    SGRN_AS_REFCOUNTED(tp_engine, "S7Server", ScriptS7Server);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour("S7Server", asBEHAVE_FACTORY, "S7Server@ f(PlcRuntime@, const string &in, uint16 = 102)",
        asFUNCTION(S7Server_factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Server", "bool start()", asFUNCTION(S7Server_start), asCALL_CDECL_OBJLAST));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Server", "void stop()", asMETHOD(ScriptS7Server, stopServer), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Server", "bool isRunning() const", asMETHOD(ScriptS7Server, isRunning), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Server", "int clientsCount() const", asMETHOD(ScriptS7Server, clientsCount), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Server", "int getCpuStatus() const", asMETHOD(ScriptS7Server, getCpuStatus), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
