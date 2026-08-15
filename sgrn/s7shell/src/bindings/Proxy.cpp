// =============================================================================
// bind_proxy.cpp — AngelScript bindings for S7ProxySession
//
// Exposes the embedded proxy session (field PLC → Hub PLC DB mirroring) to
// AngelScript scripts running inside s7shell.
//
// AS API:
//   S7ProxySession@ proxy = S7ProxySession(src_client, hub_client);
//   proxy.addMapping(srcDB, dstDB, interval_ms, size_bytes);
//   proxy.start();
//   sleep(5000);
//   proxy.stop();
// =============================================================================

#include <sgrn/s7shell/connection/ProxySession.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <angelscript.h>

namespace sgrn::s7shell::bindings
{
using sgrn::Result;
using namespace sgrn::s7shell::connection;
using namespace sgrn::s7shell::shell;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper class — manages lifetime of ScriptS7Client ref-counts alongside the
// shared_ptr<ProxySession> that owns the background ASIO loop.
// ─────────────────────────────────────────────────────────────────────────────
class ProxySessionWrapper {
public:
    ProxySessionWrapper(ScriptS7Client* tp_src, ScriptS7Client* tp_hub)
        : src_ref_(tp_src)
        , hub_ref_(tp_hub) {
        // Ensure the AS ref-counted objects aren't destroyed under us
        src_ref_->addRef();
        hub_ref_->addRef();
        session_ = std::make_shared<ProxySession>(tp_src, tp_hub);
    }

    ~ProxySessionWrapper() {
        if (session_)
            session_->stop();
        src_ref_->release();
        hub_ref_->release();
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    void addMapping(uint16_t t_src_db, uint16_t t_dst_db, int t_interval_ms, uint32_t t_size_bytes) {
        session_->addMapping(t_src_db, t_dst_db, t_interval_ms, t_size_bytes);
    }

    void start() {
        session_->start();
    }
    void stop() {
        session_->stop();
    }
    bool isRunning() const {
        return session_->isRunning();
    }

private:
    ScriptS7Client* src_ref_{nullptr};
    ScriptS7Client* hub_ref_{nullptr};
    std::shared_ptr<ProxySession> session_;
    int ref_count_{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Factory — called by AngelScript when the script calls S7ProxySession(a, b)
// ─────────────────────────────────────────────────────────────────────────────
static ProxySessionWrapper* ProxySessionWrapper_Factory(ScriptS7Client* tp_src, ScriptS7Client* tp_hub) {
    if (!tp_src || !tp_hub) {
        asIScriptContext* p_ctx = asGetActiveContext();
        if (p_ctx)
            p_ctx->SetException("S7ProxySession: src and hub S7Client handles must be non-null");
        return nullptr;
    }
    return new ProxySessionWrapper(tp_src, tp_hub);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration function — called from registration.cpp
// ─────────────────────────────────────────────────────────────────────────────
Result<void, std::string> registerProxyTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "S7ProxySession");

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour("S7ProxySession", asBEHAVE_FACTORY, "S7ProxySession@ f(S7Client@, S7Client@)",
        asFUNCTION(ProxySessionWrapper_Factory), asCALL_CDECL));

    SGRN_AS_REFCOUNTED(tp_engine, "S7ProxySession", ProxySessionWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7ProxySession", "void addMapping(uint16, uint16, int, uint = 0)", asMETHOD(ProxySessionWrapper, addMapping), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7ProxySession", "void start()", asMETHOD(ProxySessionWrapper, start), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7ProxySession", "void stop()", asMETHOD(ProxySessionWrapper, stop), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7ProxySession", "bool running() const", asMETHOD(ProxySessionWrapper, isRunning), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
