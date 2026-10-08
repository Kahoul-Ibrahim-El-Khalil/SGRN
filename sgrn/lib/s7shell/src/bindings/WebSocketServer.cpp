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
#include <sgrn/s7shell/bindings/flat_delta.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>
#include <memory>
#include <snap7.h>
#include <stdexcept>
#include <string>
#include <vector>

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

        WebSocketAdapter::BinaryWriteFn write_fn = [rt](uint16_t t_db, size_t t_offset, size_t t_size,
                                                       const uint8_t* tp_data) -> sgrn::Result<void> {
            if (rt->getMemory().writeDbMemory(t_db, t_offset, t_size, tp_data))
                return {};
            return sgrn::Result<void>::Error("writeDbMemory failed for DB" + std::to_string(t_db));
        };

        // Leaf dictionary for flat id-keyed deltas — same wire form as the
        // full gateway ({"<leaf_id>": value}). Also enables the adapter's
        // on-connect {"type":"dictionary"} decode frame. Rebuilt here so a
        // schema reload between runs never serves stale IDs. Discrete tags
        // follow the schema leaves (tags are discrete areas; DBs sweep separately).
        // Discrete-area uplink: GatewaySync control writes land in the same
        // arenas the tags live in (overlapping tags go dirty → broadcast).
        adapter_->setAreaWriteFn(
            [rt](uint16_t t_area, size_t t_offset, size_t t_size, const uint8_t* tp_data) -> sgrn::Result<void, std::string> {
                // Silent apply (see Gateway.cpp): no echo loop.
                if (auto r = rt->writeAreaMemory(t_area, t_offset, t_size, tp_data, false); r.hasError())
                    return r.error();
                return {};
            });

        leaf_dict_ = ::sgrn::gateway::twin::LeafDictionary::buildFrom(rt->getSchema());
        for (const auto& name : rt->tagNames()) {
            if (rt->describeTag(name).hasError())
                continue;
            const auto id = static_cast<::sgrn::gateway::twin::LeafId>(leaf_dict_.path_by_id.size());
            leaf_dict_.path_to_id[name] = id;
            leaf_dict_.path_by_id.push_back(name);
        }
        adapter_->setLeafDictionary(leaf_dict_);

        auto res =
            adapter_->start(t_ip, t_port, security_, &rt->getSchema(), std::move(snapshot_fn), std::move(read_fn), std::move(write_fn));
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
        if (dirty_observer_id_ == 0) {
            dirty_observer_id_ = rt->addDirtyObserver([this](uint16_t t_db, uint32_t, uint32_t) { onRuntimeDirty(t_db); });
        }
        if (tag_observer_id_ == 0) {
            tag_observer_id_ = rt->addTagDirtyObserver([this](const std::string&) { onTagDirty(); });
        }
        fmt::print(fg(fmt::color::green), "[WebSocketServer] Listening on {}:{}\n", t_ip, t_port);
        fmt::print("  WS clients receive full twin snapshot on connect,\n");
        fmt::print("  then per-field delta frames on every dirty event.\n");
        fmt::print("  Subscribe: {{\"command\":\"subscribe\",\"db\":1}}\n");
    }

    void stop() {
        if (dirty_observer_id_ != 0 || tag_observer_id_ != 0) {
            if (auto rt = runtime_ref_->getImpl()) {
                if (dirty_observer_id_ != 0)
                    rt->removeDirtyObserver(dirty_observer_id_);
                if (tag_observer_id_ != 0)
                    rt->removeTagDirtyObserver(tag_observer_id_);
            }
            dirty_observer_id_ = 0;
            tag_observer_id_ = 0;
        }
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

    /// Explicit push: broadcast a flat leaf-id delta (DBs + tags), or the
    /// full twin when clean. NOTE: no sync()/notify() here — broadcast() is
    /// the single spelling (runtime.sync() is the runtime-side verb).
    void broadcast() {
        auto rt = runtime_ref_->getImpl();
        if (!rt || !adapter_)
            return;
        rt->getMemory().processor()->processCommands();
        const std::vector<uint16_t> dirty = collectDirtyDbs(rt);
        const std::string payload = (dirty.empty() && !rt->hasDirtyTags()) ? rt->getMemory().getDigitalTwinJsonString()
                                                                           : flatDeltaForDirtyDbs(rt, leaf_dict_, dirty);
        if (!payload.empty() && payload != "{}")
            adapter_->broadcastDelta(payload);
    }

    void setAutoBroadcast(bool t_enabled) {
        auto_broadcast_ = t_enabled;
    }
    bool autoBroadcast() const {
        return auto_broadcast_;
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
    void onRuntimeDirty(uint16_t t_db) {
        if (!auto_broadcast_ || !running_ || !adapter_)
            return;
        auto rt = runtime_ref_->getImpl();
        if (!rt)
            return;
        const std::string delta = flatDeltaForDirtyDbs(rt, leaf_dict_, {t_db});
        if (!delta.empty() && delta != "{}")
            adapter_->broadcastDelta(delta);
    }

    void onTagDirty() {
        if (!auto_broadcast_ || !running_ || !adapter_)
            return;
        auto rt = runtime_ref_->getImpl();
        if (!rt)
            return;
        const std::string delta = flatDeltaForDirtyDbs(rt, leaf_dict_, {});
        if (!delta.empty() && delta != "{}")
            adapter_->broadcastDelta(delta);
    }

    PlcRuntimeWrapper* runtime_ref_{nullptr};
    SecurityManagerSptr security_;
    // Dictionary for flat id-keyed deltas, built from the runtime schema on
    // every start(). Declared before adapter_ so the adapter (which borrows
    // it via setLeafDictionary) is always destroyed first.
    ::sgrn::gateway::twin::LeafDictionary leaf_dict_;
    std::unique_ptr<WebSocketAdapter> adapter_;
    bool running_{false};
    uint16_t port_{0};
    bool auto_broadcast_{true};
    size_t dirty_observer_id_{0};
    size_t tag_observer_id_{0};
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

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("WebSocketServer", "void broadcast(const string &in)",
        asMETHODPR(WebSocketServerWrapper, broadcast, (const std::string&), void), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "void broadcast()", asMETHODPR(WebSocketServerWrapper, broadcast, (), void), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "void setAutoBroadcast(bool)", asMETHOD(WebSocketServerWrapper, setAutoBroadcast), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "bool autoBroadcast() const", asMETHOD(WebSocketServerWrapper, autoBroadcast), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WebSocketServer", "void loadPolicy(const string &in)", asMETHOD(WebSocketServerWrapper, loadPolicy), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
