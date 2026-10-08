// =============================================================================
// Gateway.cpp — unified AngelScript binding for the s7shell northbound runtime
//
// Gateway owns one NorthboundServer and mounts both the HTTP REST adapter and
// the WebSocket adapter on that server.  This is the script-facing equivalent
// of the full gateway's unified listener: one port, one Crow/asio loop, and a
// shared PlcRuntime state model.
//
//   PlcRuntime@ rt = PlcRuntime("plant.scl");
//   Gateway@ gw = Gateway(rt);
//   gw.start("0.0.0.0", 8080);
//
// HTTP: GET/POST /data, /registry, /memory/*, ...
// WS:   ws://host:8080/ws
// =============================================================================

#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/northbound/NorthboundServer.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/s7shell/bindings/flat_delta.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/bindings/tag_http.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>
#include <cstdint>
#include <memory>
#include <snap7.h>
#include <string>
#include <vector>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell; // PlcRuntimeWrapper
using sgrn::gateway::SecurityManager;
using sgrn::gateway::SecurityManagerSptr;
using sgrn::gateway::adapters::HttpAdapter;
using sgrn::gateway::adapters::northbound::NorthboundServer;
using sgrn::gateway::adapters::websocket::WebSocketAdapter;
using sgrn::gateway::database::GatewayDatabase;

class GatewayWrapper : public AngelScriptObject {
public:
    explicit GatewayWrapper(PlcRuntimeWrapper* tp_rt)
        : runtime_ref_(tp_rt)
        , security_(std::make_shared<SecurityManager>())
        , db_(std::make_shared<GatewayDatabase>())
        , http_(std::make_unique<HttpAdapter>())
        , websocket_(std::make_unique<WebSocketAdapter>()) {
        runtime_ref_->addRef();
    }

    ~GatewayWrapper() {
        stop();
        runtime_ref_->release();
    }

    void start(const std::string& t_ip, uint16_t t_port) {
        if (running_) {
            fmt::print(stderr, "[Gateway] Already running on port {}.\n", port_);
            return;
        }

        auto rt = runtime_ref_->getImpl();
        if (!rt) {
            setException("Gateway: PlcRuntime handle is null");
            return;
        }

        // Build a fresh app for every start attempt.  Crow routes must be
        // registered before the listener starts, and a failed bind leaves an
        // app that should not be reused for the next attempt.
        auto candidate = std::make_unique<NorthboundServer>();

        http_->configure(rt->getSchema(), rt->getMemory(), db_, security_);
        // Discrete tags: /tags and /tags/<name> over the same shared backing.
        hookHttpTags(http_.get(), rt);
        http_->registerRoutes(candidate->app());

        auto snapshot_fn = [rt]() -> std::string { return rt->getMemory().getDigitalTwinJsonString(); };
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

        // Leaf dictionary for flat id-keyed deltas — the same wire form the
        // full gateway emits in dictionary mode ({"<leaf_id>": value}). No
        // per-DB grouping: paths (by ID) and their deltas suffice. The
        // adapter also pushes the {"type":"dictionary"} decode frame at
        // connect time once this is set. Rebuilt on every start() so a
        // schema reload between runs never serves stale IDs.
        // Discrete-area uplink: GatewaySync control writes land in the same
        // arenas the tags live in (overlapping tags go dirty → broadcast).
        // Discrete-area uplink: GatewaySync control writes land silently
        // (no dirty marking, no broadcast) — exactly like twin DB writes.
        // Marking here would broadcast every applied uplink write back to
        // its sender, refilling the sync's publish ledger: an echo loop.
        websocket_->setAreaWriteFn(
            [rt](uint16_t t_area, size_t t_offset, size_t t_size, const uint8_t* tp_data) -> sgrn::Result<void, std::string> {
                if (auto r = rt->writeAreaMemory(t_area, t_offset, t_size, tp_data, false); r.hasError())
                    return r.error();
                return {};
            });

        leaf_dict_ = ::sgrn::gateway::twin::LeafDictionary::buildFrom(rt->getSchema());
        // Discrete (TIA-style) tags follow the schema leaves with stable,
        // sorted IDs; tag names address them on the wire. Tags cover
        // discrete areas only (DBs are DATA_BLOCK syntax), so every tag
        // listed here is a leaf the DB sweep cannot see.
        for (const auto& name : rt->tagNames()) {
            if (rt->describeTag(name).hasError())
                continue;
            const auto id = static_cast<::sgrn::gateway::twin::LeafId>(leaf_dict_.path_by_id.size());
            leaf_dict_.path_to_id[name] = id;
            leaf_dict_.path_by_id.push_back(name);
        }
        websocket_->setLeafDictionary(leaf_dict_);
        websocket_->configure(security_, &rt->getSchema(), std::move(snapshot_fn), std::move(read_fn), std::move(write_fn));
        websocket_->registerRoutes(candidate->app());

        if (auto res = candidate->start(t_ip, t_port); res.hasError()) {
            websocket_->stop();
            setException("Gateway: " + res.error());
            fmt::print(stderr, fg(fmt::color::red), "[Gateway] start failed: {}\n", res.error());
            return;
        }

        server_ = std::move(candidate);
        running_ = true;
        port_ = t_port;

        // ── Dual-mode auto-broadcast ─────────────────────────────────────
        // Subscribe to the shared PlcRuntime dirty ledgers (DB + discrete
        // tags) so every markDirty / tag write (script field write,
        // DataBlock::write, tagPut, GatewaySync delta, SimEngine tick,
        // runtime.sync()) immediately streams a flat delta frame
        // to WS clients. This is the "event registered -> broadcast" half;
        // the explicit half is broadcast() below.
        // Unsubscribed in stop() so a restart never double-subscribes.
        if (dirty_observer_id_ == 0) {
            dirty_observer_id_ = rt->addDirtyObserver([this](uint16_t t_db, uint32_t, uint32_t) { onRuntimeDirty(t_db); });
        }
        if (tag_observer_id_ == 0) {
            tag_observer_id_ = rt->addTagDirtyObserver([this](const std::string&) { onTagDirty(); });
        }

        fmt::print(fg(fmt::color::green), "[Gateway] Listening on {}:{}\n", t_ip, t_port);
        fmt::print("  HTTP: http://{}:{}/data/ and /registry\n", t_ip, t_port);
        fmt::print("  Tags: http://{}:{}/tags/<name> (GET/POST)\n", t_ip, t_port);
        fmt::print("  WS:   ws://{}:{}/ws (flat leaf-id deltas + dictionary)\n", t_ip, t_port);
    }

    void stop() {
        // Drop the dirty subscriptions first so no in-flight script write can
        // re-enter broadcastDelta while the listener is tearing down.
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
        if (!running_ && !server_)
            return;
        const bool was_running = running_;

        // Stop the shared listener before releasing adapter subscriptions or
        // route state.  In-flight handlers borrow both adapters.
        if (server_)
            server_->stop();
        websocket_->stop();
        http_->stop();
        server_.reset();
        running_ = false;
        if (was_running) {
            fmt::print(fg(fmt::color::yellow), "[Gateway] Stopped.\n");
        }
    }

    bool isRunning() const {
        return running_;
    }

    /// Push an out-of-band JSON notification to all connected WS clients.
    void broadcast(const std::string& t_json) {
        websocket_->broadcastDelta(t_json);
    }

    /// Explicit push: snapshot dirty DBs/tags and broadcast them immediately
    /// as a flat leaf-id delta (same wire form as the full gateway). With no
    /// dirty regions this sends the full twin so `gt.broadcast()` after
    /// `runtime.inlet_separation.put()` always pushes something useful.
    /// Does NOT consume the PlcRuntime dirty ledger, so a GatewaySync
    /// attached to the same runtime still publishes retries.
    /// NOTE: no sync()/notify() here — broadcast() is the single spelling
    /// (runtime.sync() is the runtime-side verb).
    void broadcast() {
        auto rt = runtime_ref_->getImpl();
        if (!rt) {
            setException("Gateway: PlcRuntime handle is null");
            return;
        }
        rt->getMemory().processor()->processCommands();
        const std::vector<uint16_t> dirty = collectDirtyDbs(rt);
        // Flat id-keyed delta (DBs + discrete tags); full twin when clean.
        const std::string payload = (dirty.empty() && !rt->hasDirtyTags()) ? rt->getMemory().getDigitalTwinJsonString()
                                                                           : flatDeltaForDirtyDbs(rt, leaf_dict_, dirty);
        if (payload.empty() || payload == "{}") {
            fmt::print(fg(fmt::color::yellow), "[Gateway] Nothing to broadcast (twin empty).\n");
            return;
        }
        websocket_->broadcastDelta(payload);
        if (!dirty.empty())
            fmt::print(fg(fmt::color::green), "[Gateway] Broadcast flat delta for {} DB(s).\n", dirty.size());
        else
            fmt::print(fg(fmt::color::green), "[Gateway] Broadcast full twin snapshot.\n");
    }

    void setAutoBroadcast(bool t_enabled) {
        auto_broadcast_ = t_enabled;
        fmt::print(fg(fmt::color::cyan), "[Gateway] Auto-broadcast {}.\n", t_enabled ? "enabled" : "disabled");
    }
    bool autoBroadcast() const {
        return auto_broadcast_;
    }

    void loadPolicy(const std::string& t_path) {
        auto res = security_->loadPolicyScript(t_path);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[Gateway] loadPolicy failed: {}\n", res.error());
            setException("Gateway.loadPolicy: " + res.error());
        } else {
            fmt::print(fg(fmt::color::cyan), "[Gateway] Security policy loaded from {}\n", t_path);
        }
    }

private:
    static void setException(const std::string& t_message) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException(t_message.c_str());
    }

    /// Auto-broadcast path: one dirty event -> one flat delta frame.
    void onRuntimeDirty(uint16_t t_db) {
        if (!auto_broadcast_ || !running_)
            return;
        auto rt = runtime_ref_->getImpl();
        if (!rt)
            return;
        const std::string delta = flatDeltaForDirtyDbs(rt, leaf_dict_, {t_db});
        if (delta.empty() || delta == "{}")
            return;
        websocket_->broadcastDelta(delta);
    }

    /// Auto-broadcast path for discrete tags: drain all pending tag leaves
    /// into one flat frame (coalesces bursts from the same scan cycle).
    void onTagDirty() {
        if (!auto_broadcast_ || !running_)
            return;
        auto rt = runtime_ref_->getImpl();
        if (!rt)
            return;
        const std::string delta = flatDeltaForDirtyDbs(rt, leaf_dict_, {});
        if (delta.empty() || delta == "{}")
            return;
        websocket_->broadcastDelta(delta);
    }

    PlcRuntimeWrapper* runtime_ref_{nullptr};
    SecurityManagerSptr security_;
    std::shared_ptr<GatewayDatabase> db_;
    // Dictionary for flat id-keyed deltas, built from the runtime schema on
    // every start(). Declared before websocket_ so the adapter (which borrows
    // it via setLeafDictionary) is always destroyed first.
    ::sgrn::gateway::twin::LeafDictionary leaf_dict_;
    std::unique_ptr<HttpAdapter> http_;
    std::unique_ptr<WebSocketAdapter> websocket_;
    std::unique_ptr<NorthboundServer> server_;
    bool running_{false};
    uint16_t port_{0};
    /// Dual-mode switch: true = every markDirty auto-broadcasts a delta;
    /// false = only explicit broadcast() pushes.
    bool auto_broadcast_{true};
    size_t dirty_observer_id_{0};
    size_t tag_observer_id_{0};
};

static GatewayWrapper* Gateway_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("Gateway: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new GatewayWrapper(tp_rt);
}

Result<void, std::string> registerGatewayTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "Gateway");
    SGRN_AS_REFCOUNTED(tp_engine, "Gateway", GatewayWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "Gateway", asBEHAVE_FACTORY, "Gateway@ f(PlcRuntime@)", asFUNCTION(Gateway_Factory), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "Gateway", "void start(const string &in, uint16 port = 8080)", asMETHOD(GatewayWrapper, start), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Gateway", "void stop()", asMETHOD(GatewayWrapper, stop), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Gateway", "bool isRunning() const", asMETHOD(GatewayWrapper, isRunning), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "Gateway", "void broadcast(const string &in)", asMETHODPR(GatewayWrapper, broadcast, (const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("Gateway", "void broadcast()", asMETHODPR(GatewayWrapper, broadcast, (), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "Gateway", "void setAutoBroadcast(bool)", asMETHOD(GatewayWrapper, setAutoBroadcast), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("Gateway", "bool autoBroadcast() const", asMETHOD(GatewayWrapper, autoBroadcast), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "Gateway", "void loadPolicy(const string &in)", asMETHOD(GatewayWrapper, loadPolicy), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
