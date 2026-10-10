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
//   sync.connect("ws://192.168.1.1:8080/ws");
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
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::connection;
using namespace sgrn::s7shell::shell;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper — ref-counted AS object owning a GatewaySync instance
// ─────────────────────────────────────────────────────────────────────────────
class GatewaySyncWrapper : public AngelScriptObject {
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

    void subscribeDb(uint16_t t_db) {
        sync_->subscribeDb(t_db);
    }
    void unsubscribeDb(uint16_t t_db) {
        sync_->unsubscribeDb(t_db);
    }
    void publishOnDirty(bool t_enabled) {
        sync_->publishOnDirty(t_enabled);
    }
    void useBinary(bool t_enabled) {
        sync_->useBinary(t_enabled);
    }

    bool connect(const std::string& t_ws_url) {
        return sync_->connect(t_ws_url);
    }
    void sync() {
        sync_->sync();
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
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────
static GatewaySyncWrapper* GatewaySyncWrapper_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        asIScriptContext* p_ctx = asGetActiveContext();
        if (p_ctx)
            p_ctx->SetException("GatewayClient: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new GatewaySyncWrapper(tp_rt);
}

class GatewayServerWrapper : public AngelScriptObject {
public:
    explicit GatewayServerWrapper(PlcRuntimeWrapper* tp_rt)
        : runtime_ref_(tp_rt) {
        runtime_ref_->addRef();
        server_ = std::make_unique<GatewayServer>(tp_rt->getImpl());
    }

    ~GatewayServerWrapper() {
        server_.reset();
        runtime_ref_->release();
    }

    bool start(uint16_t t_port = 8000) {
        return server_->start(t_port);
    }
    void stop() {
        server_->stop();
    }
    bool isRunning() const {
        return server_->isRunning();
    }
    void broadcast() {
        server_->broadcast();
    }
    std::string lastError() const {
        return server_->getLastError();
    }

private:
    PlcRuntimeWrapper* runtime_ref_{nullptr};
    std::unique_ptr<GatewayServer> server_;
};

static GatewayServerWrapper* GatewayServerWrapper_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        asIScriptContext* p_ctx = asGetActiveContext();
        if (p_ctx)
            p_ctx->SetException("GatewayServer: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new GatewayServerWrapper(tp_rt);
}

static Result<void, std::string> registerClientMethods(asIScriptEngine* tp_engine, const char* t_type_name) {
    int r = 0;
    SGRN_AS_TYPE(tp_engine, t_type_name);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(t_type_name, asBEHAVE_FACTORY, fmt::format("{}@ f(PlcRuntime@)", t_type_name).c_str(),
        asFUNCTION(GatewaySyncWrapper_Factory), asCALL_CDECL));

    SGRN_AS_REFCOUNTED(tp_engine, t_type_name, GatewaySyncWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        t_type_name, "void subscribeDb(uint16)", asMETHOD(GatewaySyncWrapper, subscribeDb), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        t_type_name, "void unsubscribeDb(uint16)", asMETHOD(GatewaySyncWrapper, unsubscribeDb), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        t_type_name, "void publishOnDirty(bool)", asMETHOD(GatewaySyncWrapper, publishOnDirty), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod(t_type_name, "void useBinary(bool)", asMETHOD(GatewaySyncWrapper, useBinary), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        t_type_name, "bool connect(const string &in)", asMETHOD(GatewaySyncWrapper, connect), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(t_type_name, "void sync()", asMETHOD(GatewaySyncWrapper, sync), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod(t_type_name, "void disconnect()", asMETHOD(GatewaySyncWrapper, disconnect), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod(t_type_name, "bool connected() const", asMETHOD(GatewaySyncWrapper, connected), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod(t_type_name, "string lastError() const", asMETHOD(GatewaySyncWrapper, lastError), asCALL_THISCALL));

    return {};
}

static Result<void, std::string> registerServerMethods(asIScriptEngine* tp_engine) {
    int r = 0;
    SGRN_AS_TYPE(tp_engine, "GatewayServer");

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "GatewayServer", asBEHAVE_FACTORY, "GatewayServer@ f(PlcRuntime@)", asFUNCTION(GatewayServerWrapper_Factory), asCALL_CDECL));

    SGRN_AS_REFCOUNTED(tp_engine, "GatewayServer", GatewayServerWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewayServer", "bool start(uint16 = 8000)", asMETHOD(GatewayServerWrapper, start), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("GatewayServer", "void stop()", asMETHOD(GatewayServerWrapper, stop), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewayServer", "bool isRunning() const", asMETHOD(GatewayServerWrapper, isRunning), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("GatewayServer", "void broadcast()", asMETHOD(GatewayServerWrapper, broadcast), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "GatewayServer", "string lastError() const", asMETHOD(GatewayServerWrapper, lastError), asCALL_THISCALL));

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────
Result<void, std::string> registerGatewaySyncTypes(asIScriptEngine* tp_engine) {
    SGRN_RETURN_IF(auto r1 = registerClientMethods(tp_engine, "GatewayClient"); r1.hasError(), r1.error());
    SGRN_RETURN_IF(auto r2 = registerClientMethods(tp_engine, "GatewaySync"); r2.hasError(), r2.error());
    SGRN_RETURN_IF(auto r3 = registerServerMethods(tp_engine); r3.hasError(), r3.error());
    return {};
}

} // namespace sgrn::s7shell::bindings
