// =============================================================================
// WebSocketServer.cpp — AngelScript binding for the gateway's WebSocketAdapter
//
// Exposes the gateway's WebSocketAdapter directly to scripts.
// Because WebSocketAdapter subscribes to TelemetryBroker::instance() — the
// same singleton that PersistenceBridge already publishes into — every dirty
// event (script write, db.get() diff, SimEngine tick) automatically streams
// as a JSON delta frame to all connected WebSocket clients.  Zero extra wiring.
//
//   PlcRuntime@ rt = PlcRuntime("plant.scl");
//   Persistence@ pers = Persistence(rt, "./data/");
//   pers.start();
//
//   WebSocketServer@ ws = WebSocketServer(rt);
//   ws.start("0.0.0.0", 9001);
//
//   // WS clients receive a full twin snapshot on connect, then per-field
//   // delta frames on every change.  Client protocol is identical to the
//   // full gateway WebSocket adapter.
//
//   ws.broadcast("{\"alert\":\"manual override\"}");
//   print("Clients: " + ws.clientCount());
//   ws.stop();
//
// =============================================================================

#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>
#include <memory>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell; // PlcRuntimeWrapper
using sgrn::gateway::SecurityManager;
using sgrn::gateway::SecurityManagerSptr;
using sgrn::gateway::adapters::websocket::WebSocketAdapter;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper
// ─────────────────────────────────────────────────────────────────────────────

class WebSocketServerWrapper {
public:
    explicit WebSocketServerWrapper(PlcRuntimeWrapper* tp_rt)
        : runtime_ref_(tp_rt)
        , security_(std::make_shared<SecurityManager>()) {
        runtime_ref_->addRef();
        adapter_ = std::make_unique<WebSocketAdapter>();
    }

    ~WebSocketServerWrapper() {
        if (running_)
            stop();
        runtime_ref_->release();
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void start(const std::string& t_ip, uint16_t t_port) {
        if (running_) {
            fmt::print(stderr, "[WebSocketServer] Already running on port {}.\n", port_);
            return;
        }
        auto rt = runtime_ref_->getImpl();
        if (!rt) {
            if (auto* p_ctx = asGetActiveContext())
                p_ctx->SetException("WebSocketServer: PlcRuntime handle is null");
            return;
        }

        // Full-snapshot provider: seeds newly connected clients with the
        // current in-memory twin state (same as the gateway does after restart).
        // getDigitalTwinJsonString() returns the schema-aware JSON of all DBs.
        auto snapshot_fn = [rt]() -> std::string { return rt->getMemory().getDigitalTwinJsonString(); };

        // Binary read function: allows WS clients to request raw DB byte regions
        // via the binary subscription protocol (same as the gateway).
        WebSocketAdapter::BinaryReadFn read_fn = [rt](
                                                     uint16_t t_db, size_t t_offset, size_t t_size, uint8_t* tp_out) -> sgrn::Result<void> {
            if (rt->getMemory().readDbMemory(t_db, t_offset, t_size, tp_out))
                return {};
            return sgrn::Result<void>::Error("readDbMemory failed for DB" + std::to_string(t_db));
        };

        auto res = adapter_->start(t_ip, t_port, security_, &rt->getSchema(), std::move(snapshot_fn), std::move(read_fn));
        // WebSocketAdapter::start() internally subscribes to TelemetryBroker::instance().
        // From this point, every markDirty event flows:
        //   PersistenceBridge → TelemetryBroker → WebSocketAdapter → WS clients

        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[WebSocketServer] start failed: {}\n", res.error());
            if (auto* p_ctx = asGetActiveContext())
                p_ctx->SetException(("WebSocketServer: " + res.error()).c_str());
            return;
        }

        running_ = true;
        port_ = t_port;
        fmt::print(fg(fmt::color::green), "[WebSocketServer] Listening on {}:{}\n", t_ip, t_port);
        fmt::print("  WS clients receive full twin snapshot on connect,\n");
        fmt::print("  then per-field delta frames on every dirty event.\n");
        fmt::print("  Subscribe: {{\"command\":\"subscribe\",\"db\":1}}\n");
    }

    void stop() {
        if (!running_)
            return;
        adapter_->stop();
        running_ = false;
        fmt::print(fg(fmt::color::yellow), "[WebSocketServer] Stopped.\n");
    }

    bool isRunning() const {
        return running_;
    }

    /// Push an arbitrary JSON string to all connected clients immediately.
    /// Useful for out-of-band notifications (alerts, heartbeats, etc.).
    void broadcast(const std::string& t_json) {
        if (adapter_)
            adapter_->broadcastDelta(t_json);
    }

    // ── Security ──────────────────────────────────────────────────────────────

    /// Load a gateway-compatible security policy script (security.as).
    void loadPolicy(const std::string& t_path) {
        auto res = security_->loadPolicyScript(t_path);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[WebSocketServer] loadPolicy failed: {}\n", res.error());
            if (auto* p_ctx = asGetActiveContext())
                p_ctx->SetException(("WebSocketServer.loadPolicy: " + res.error()).c_str());
        } else {
            fmt::print(fg(fmt::color::cyan), "[WebSocketServer] Security policy loaded from {}\n", t_path);
        }
    }

private:
    PlcRuntimeWrapper* runtime_ref_{nullptr};
    SecurityManagerSptr security_;
    std::unique_ptr<WebSocketAdapter> adapter_;
    bool running_{false};
    uint16_t port_{0};
    int ref_count_{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

static WebSocketServerWrapper* WebSocketServer_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("WebSocketServer: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new WebSocketServerWrapper(tp_rt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

Result<void, std::string> registerWebSocketServerTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "WebSocketServer");
    SGRN_AS_REFCOUNTED(tp_engine, "WebSocketServer", WebSocketServerWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "WebSocketServer", asBEHAVE_FACTORY, "WebSocketServer@ f(PlcRuntime@)", asFUNCTION(WebSocketServer_Factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "void start(const string &in, uint16 port = 9001)", asMETHOD(WebSocketServerWrapper, start), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("WebSocketServer", "void stop()", asMETHOD(WebSocketServerWrapper, stop), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "bool isRunning() const", asMETHOD(WebSocketServerWrapper, isRunning), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "void broadcast(const string &in)", asMETHOD(WebSocketServerWrapper, broadcast), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "void loadPolicy(const string &in)", asMETHOD(WebSocketServerWrapper, loadPolicy), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
