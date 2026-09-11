// =============================================================================
// bind_gateway_sync.cpp — AngelScript bindings for GatewaySync
//
// Exposes the WebSocket-based gateway dirty-tag synchronization client to
// AngelScript scripts running inside s7shell.
//
// AS API:
//   PlcRuntime@ rt = PlcRuntime("schema.scl");
//   GatewaySync@ sync = GatewaySync(rt);
//   sync.subscribeDb(1);            // optional: only sync DB 1
//   sync.connect("ws://192.168.1.1:8080");
//   while (sync.connected()) {
//       sleep(1000);
//   }
//   print(sync.lastError());
// =============================================================================

#include <sgrn/s7shell/connection/GatewaySync.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <angelscript.h>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::connection;
using namespace sgrn::s7shell::shell;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper — ref-counted AS object owning a GatewaySync instance
// ─────────────────────────────────────────────────────────────────────────────
class GatewaySyncWrapper {
public:
    explicit GatewaySyncWrapper(PlcRuntimeWrapper* tp_rt)
        : runtime_ref_(tp_rt) {
        runtime_ref_->addRef();
        sync_ = std::make_unique<GatewaySync>(tp_rt->getImpl());
    }

    ~GatewaySyncWrapper() {
        sync_.reset();
        runtime_ref_->release();
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    void subscribeDb(uint16_t t_db) {
        sync_->subscribeDb(t_db);
    }
    void unsubscribeDb(uint16_t t_db) {
        sync_->unsubscribeDb(t_db);
    }
    void publishOnDirty(bool t_enabled) {
        sync_->publishOnDirty(t_enabled);
    }

    bool connect(const std::string& t_ws_url) {
        return sync_->connect(t_ws_url);
    }
    void disconnect() {
        sync_->disconnect();
    }
    bool connected() const {
        return sync_->isConnected();
    }
    std::string lastError() const {
        return sync_->getLastError();
    }

private:
    PlcRuntimeWrapper* runtime_ref_{nullptr};
    std::unique_ptr<GatewaySync> sync_;
    int ref_count_{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────
static GatewaySyncWrapper* GatewaySyncWrapper_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        asIScriptContext* p_ctx = asGetActiveContext();
        if (p_ctx)
            p_ctx->SetException("GatewaySync: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new GatewaySyncWrapper(tp_rt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────
Result<void, std::string> registerGatewaySyncTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "GatewaySync");

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "GatewaySync", asBEHAVE_FACTORY, "GatewaySync@ f(PlcRuntime@)", asFUNCTION(GatewaySyncWrapper_Factory), asCALL_CDECL));

    SGRN_AS_REFCOUNTED(tp_engine, "GatewaySync", GatewaySyncWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewaySync", "void subscribeDb(uint16)", asMETHOD(GatewaySyncWrapper, subscribeDb), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewaySync", "void unsubscribeDb(uint16)", asMETHOD(GatewaySyncWrapper, unsubscribeDb), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewaySync", "void publishOnDirty(bool)", asMETHOD(GatewaySyncWrapper, publishOnDirty), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewaySync", "bool connect(const string &in)", asMETHOD(GatewaySyncWrapper, connect), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("GatewaySync", "void disconnect()", asMETHOD(GatewaySyncWrapper, disconnect), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("GatewaySync", "bool connected() const", asMETHOD(GatewaySyncWrapper, connected), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewaySync", "string lastError() const", asMETHOD(GatewaySyncWrapper, lastError), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
