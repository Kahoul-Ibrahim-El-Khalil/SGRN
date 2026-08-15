#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/SchemaVM.hpp>
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
#include <sgrn/s7shell/utils/PlcSimClock.hpp>
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

#ifdef SGRN_HAS_OPC
Result<void, std::string> registerOpcUaServer(asIScriptEngine* tp_engine);
#endif
} // namespace sgrn::s7shell::bindings

namespace sgrn::s7shell::shell
{

asIScriptEngine* p_g_as_engine = nullptr;

Result<void, std::string> registerS7Shell(asIScriptEngine* tp_engine) {
    p_g_as_engine = tp_engine;

    SGRN_REGISTER_MODULE(registerS7Types(tp_engine));
    SGRN_REGISTER_MODULE(registerS7Globals(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerPlcRuntimeTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerProxyTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerGatewaySyncTypes(tp_engine));
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerS7Server(tp_engine));
#ifdef SGRN_HAS_OPC
    SGRN_REGISTER_MODULE(sgrn::s7shell::bindings::registerOpcUaServer(tp_engine));
#endif
    return {};
}

} // namespace sgrn::s7shell::shell
