#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/scripting/ScriptHost.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptAsync.hpp>
#include <sgrn/s7shell/facades/ScriptBlocks.hpp>
#include <sgrn/s7shell/facades/ScriptConnectionProxy.hpp>
#include <sgrn/s7shell/facades/ScriptDiagnostics.hpp>
#include <sgrn/s7shell/facades/ScriptMemory.hpp>
#include <sgrn/s7shell/facades/ScriptPlcControl.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptFieldProxy.hpp>
#include <sgrn/s7shell/script/ScriptHexTable.hpp>
#include <sgrn/s7shell/script/ScriptPathBatch.hpp>
#include <sgrn/s7shell/script/ScriptSchemaStore.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/plcsim/utils/PlcSimClock.hpp>
#include <angelscript.h>
#include <ctime>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <snap7.h>

namespace sgrn::s7shell::bindings
{
using sgrn::Result;

Result<void, std::string> registerProxyTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerGatewaySyncTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerPlcRuntimeTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerS7Server(asIScriptEngine* tp_engine);
Result<void, std::string> registerPersistenceTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerSimulationTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerReplayTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerHttpServerTypes(asIScriptEngine* tp_engine);
Result<void, std::string> registerWebSocketServerTypes(asIScriptEngine* tp_engine);

#ifdef SGRN_HAS_OPC
Result<void, std::string> registerOpcUaServer(asIScriptEngine* tp_engine);
#endif
} // namespace sgrn::s7shell::bindings

namespace sgrn::s7shell::shell
{

asIScriptEngine* p_g_as_engine = nullptr;

Result<void, std::string> registerS7Shell(asIScriptEngine* tp_engine) {
    p_g_as_engine = tp_engine;

    // Arm the plcsim schema-reload hook: PlcRuntime::{loadSclSchema,
    // loadJsonSchema} invoke this after loading so script-visible types stay
    // in sync — the same re-registration the runtime used to do inline.
    // Reads p_g_as_engine lazily so a later engine swap is honored.
    ::sgrn::plcsim::runtime::g_on_schema_loaded = [](::sgrn::plcsim::runtime::PlcRuntime& t_rt) {
        sgrn::scripting::ScriptHost host(p_g_as_engine);
        registerSchemaTypes(host, t_rt.getSchema());
        registerDbPropertyAccessors(host, t_rt.getSchema());
    };

    SGRN_REGISTER_MODULE(registerS7Types(tp_engine));
    SGRN_REGISTER_MODULE(registerS7Globals(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerPlcRuntimeTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerPersistenceTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerSimulationTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerReplayTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerProxyTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerGatewaySyncTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerS7Server(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerHttpServerTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerWebSocketServerTypes(tp_engine));
#ifdef SGRN_HAS_OPC
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerOpcUaServer(tp_engine));
#endif
    return {};
}

} // namespace sgrn::s7shell::shell
