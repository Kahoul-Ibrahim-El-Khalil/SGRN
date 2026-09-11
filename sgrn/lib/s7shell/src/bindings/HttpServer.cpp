// =============================================================================
// HttpServer.cpp — AngelScript binding for the gateway's HttpAdapter
//
// Exposes the gateway's battle-tested HTTP REST server directly to scripts:
//
//   PlcRuntime@ rt = PlcRuntime("plant.scl");
//   HttpServer@ http = HttpServer(rt);
//   http.start("0.0.0.0", 8080);
//
//   // GET  http://localhost:8080/data/motor.speed  → "1450.0"
//   // POST http://localhost:8080/data/motor.setpoint  {"value": 1500}
//   // GET  http://localhost:8080/data/             → full twin JSON
//
//   http.stop();
//
// The server wires PlcRuntime::getMemory() and PlcRuntime::getSchema()
// into HttpAdapter — no new endpoint logic required.
//
// Security defaults to Relaxed (allow-all).  Call http.loadPolicy("sec.as")
// to apply the same ACL policy files used by the full gateway.
//
// GatewayDatabase: a default-constructed (uninitialized) instance is used.
// The /db/connections, /db/history, /db/sessions, /db/logs endpoints will
// return HTTP 500 {"error":"Database not initialized"} — acceptable for
// scripting use where SQL audit logging is not needed.
// =============================================================================

#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell; // PlcRuntimeWrapper
using sgrn::gateway::SecurityManager;
using sgrn::gateway::SecurityManagerSptr;
using sgrn::gateway::adapters::HttpAdapter;
using sgrn::gateway::database::GatewayDatabase;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper
// ─────────────────────────────────────────────────────────────────────────────

class HttpServerWrapper {
public:
    explicit HttpServerWrapper(PlcRuntimeWrapper* tp_rt)
        : runtime_ref_(tp_rt)
        , security_(std::make_shared<SecurityManager>())
        , db_(std::make_shared<GatewayDatabase>())
    // GatewayDatabase is intentionally left uninitialized (no SQLite file).
    // All /db/* handlers check the Result and return HTTP 500 gracefully.
    {
        runtime_ref_->addRef();
        adapter_ = std::make_unique<HttpAdapter>();
    }

    ~HttpServerWrapper() {
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
            fmt::print(stderr, "[HttpServer] Already running on port {}.\n", port_);
            return;
        }
        auto rt = runtime_ref_->getImpl();
        if (!rt) {
            if (auto* p_ctx = asGetActiveContext())
                p_ctx->SetException("HttpServer: PlcRuntime handle is null");
            return;
        }

        auto res = adapter_->start(t_ip, t_port, rt->getSchema(), rt->getMemory(), db_, security_);

        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[HttpServer] start failed: {}\n", res.error());
            if (auto* p_ctx = asGetActiveContext())
                p_ctx->SetException(("HttpServer: " + res.error()).c_str());
            return;
        }

        running_ = true;
        port_ = t_port;
        fmt::print(fg(fmt::color::green), "[HttpServer] Listening on {}:{}\n", t_ip, t_port);
        fmt::print("  GET  http://{}:{}/data/<path>   - read field / subtree / full twin\n", t_ip, t_port);
        fmt::print("  POST http://{}:{}/data/<path>   - write via JSON\n", t_ip, t_port);
        fmt::print("  GET  http://{}:{}/registry      - schema registry\n", t_ip, t_port);
    }

    void stop() {
        if (!running_)
            return;
        adapter_->stop();
        running_ = false;
        fmt::print(fg(fmt::color::yellow), "[HttpServer] Stopped.\n");
    }

    bool isRunning() const {
        return running_;
    }

    // ── Security ──────────────────────────────────────────────────────────────

    /// Load a gateway-compatible security policy script (security.as).
    /// Overrides the default Relaxed (allow-all) policy.
    void loadPolicy(const std::string& t_path) {
        auto res = security_->loadPolicyScript(t_path);
        if (res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[HttpServer] loadPolicy failed: {}\n", res.error());
            if (auto* p_ctx = asGetActiveContext())
                p_ctx->SetException(("HttpServer.loadPolicy: " + res.error()).c_str());
        } else {
            fmt::print(fg(fmt::color::cyan), "[HttpServer] Security policy loaded from {}\n", t_path);
        }
    }

private:
    PlcRuntimeWrapper* runtime_ref_{nullptr};
    SecurityManagerSptr security_;
    std::shared_ptr<GatewayDatabase> db_;
    std::unique_ptr<HttpAdapter> adapter_;
    bool running_{false};
    uint16_t port_{0};
    int ref_count_{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

static HttpServerWrapper* HttpServer_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("HttpServer: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new HttpServerWrapper(tp_rt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

Result<void, std::string> registerHttpServerTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "HttpServer");
    SGRN_AS_REFCOUNTED(tp_engine, "HttpServer", HttpServerWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "HttpServer", asBEHAVE_FACTORY, "HttpServer@ f(PlcRuntime@)", asFUNCTION(HttpServer_Factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "HttpServer", "void start(const string &in, uint16 port = 8080)", asMETHOD(HttpServerWrapper, start), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("HttpServer", "void stop()", asMETHOD(HttpServerWrapper, stop), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("HttpServer", "bool isRunning() const", asMETHOD(HttpServerWrapper, isRunning), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "HttpServer", "void loadPolicy(const string &in)", asMETHOD(HttpServerWrapper, loadPolicy), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
