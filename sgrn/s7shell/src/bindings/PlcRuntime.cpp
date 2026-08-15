// =============================================================================
// bind_plc_runtime.cpp — AngelScript bindings for PlcRuntime
//
// Exposes the schema+memory owner extracted out of ScriptS7Connection so
// scripts can allocate one explicitly and share it across multiple S7Client
// instances (and, in future, other protocol endpoints):
//
// AS API:
//   PlcRuntime@ rt = PlcRuntime("plant.scl");
//   S7Client@ field = S7Client("192.168.10.5", 0, 1, rt);
//   S7Client@ hub   = S7Client("192.168.1.1",  0, 1, rt);
//
// A PlcRuntime@ constructed with no arguments starts empty (no schema
// loaded yet) — call loadSclSchema()/loadJsonSchema() on it, or on any
// S7Client attached to it, to load one afterwards.
// =============================================================================

#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>

#include <angelscript.h>

namespace sgrn::s7shell::bindings
{
using sgrn::Result;
using namespace sgrn::s7shell::shell;
using ::sgrn::s7shell::runtime::PlcRuntime;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper class — gives the C++ std::shared_ptr<PlcRuntime> an AngelScript
// intrusive ref-count so it can be registered as a normal AS ref type
// ("PlcRuntime@"), mirroring the pattern used for ProxySession/GatewaySync
// in bind_proxy.cpp / bind_gateway_sync.cpp.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Factories
// ─────────────────────────────────────────────────────────────────────────────
static PlcRuntimeWrapper* PlcRuntime_Factory() {
    return new PlcRuntimeWrapper(PlcRuntime::empty());
}

static PlcRuntimeWrapper* PlcRuntime_FactoryFromSchema(const std::string& t_path) {
    return new PlcRuntimeWrapper(PlcRuntime::fromSclSchema(t_path));
}

// S7Client@ f(const string &in, int, int, PlcRuntime@) — attaches the new
// client to an existing runtime instead of allocating a private one.
static ScriptS7Client* S7Client_factoryWithRuntime(
    const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port, PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("S7Client: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new ScriptS7Client(t_ip, t_rack, t_slot, t_port, tp_rt->getImpl());
}

static PlcRuntimeWrapper* S7Client_getRuntime(ScriptS7Client* tp_client) {
    if (!tp_client)
        return nullptr;
    return new PlcRuntimeWrapper(tp_client->getRuntime());
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration function — called from registration.cpp
// ─────────────────────────────────────────────────────────────────────────────
Result<void, std::string> registerPlcRuntimeTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "PlcRuntime");

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "PlcRuntime", asBEHAVE_FACTORY, "PlcRuntime@ f()", asFUNCTION(PlcRuntime_Factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "PlcRuntime", asBEHAVE_FACTORY, "PlcRuntime@ f(const string &in)", asFUNCTION(PlcRuntime_FactoryFromSchema), asCALL_CDECL));

    SGRN_AS_REFCOUNTED(tp_engine, "PlcRuntime", PlcRuntimeWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "void loadSclSchema(const string &in)", asMETHOD(PlcRuntimeWrapper, loadSclSchema), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "void loadJsonSchema(const string &in)", asMETHOD(PlcRuntimeWrapper, loadJsonSchema), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "void registerDb(uint16, uint, const string &in = \"\")", asMETHOD(PlcRuntimeWrapper, registerDb), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "void registerUdt(const string &in, uint)", asMETHOD(PlcRuntimeWrapper, registerUdt), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("PlcRuntime",
        "void addUdtField(const string &in, const string &in, const string &in, uint, uint16 = 1)",
        asMETHOD(PlcRuntimeWrapper, addUdtField), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "void loadRegistry(const string &in)", asMETHOD(PlcRuntimeWrapper, loadRegistry), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "bool hasDirty(uint16) const", asMETHOD(PlcRuntimeWrapper, hasDirty), asCALL_THISCALL));

    // ── Virtual PLC memory manipulation ────────────────────────────────────
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "DataBlock@ db(uint16)", asMETHODPR(PlcRuntimeWrapper, db, (uint16_t), ScriptDataBlock*), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "bool set(uint16, const string &in, const string &in)", asMETHOD(PlcRuntimeWrapper, set), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "string get(uint16, const string &in) const", asMETHOD(PlcRuntimeWrapper, get), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "string getJson(uint16) const", asMETHOD(PlcRuntimeWrapper, getJson), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "PlcRuntime", "bool setBit(uint16, uint, int, bool)", asMETHOD(PlcRuntimeWrapper, setBit), asCALL_THISCALL));

    // ── Schema layout inspection ──────────────────────────────────────────────
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("PlcRuntime", "void DBS()", asMETHOD(PlcRuntimeWrapper, DBS), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("PlcRuntime", "void UDTS()", asMETHOD(PlcRuntimeWrapper, UDTS), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "PlcRuntime@ get_rt() const", asFUNCTION(S7Client_getRuntime), asCALL_CDECL_OBJFIRST));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "PlcRuntime@ runtime() const", asFUNCTION(S7Client_getRuntime), asCALL_CDECL_OBJFIRST));

    // S7Client(ip, rack, slot, PlcRuntime@) — shares an existing runtime
    // instead of allocating a private one.
    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour("S7Client", asBEHAVE_FACTORY,
        "S7Client@ f(const string &in, int, int, uint16, PlcRuntime@)", asFUNCTION(S7Client_factoryWithRuntime), asCALL_CDECL));

    return {};
}

} // namespace sgrn::s7shell::bindings
