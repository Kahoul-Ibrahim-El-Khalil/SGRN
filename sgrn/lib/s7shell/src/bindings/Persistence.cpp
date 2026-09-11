// =============================================================================
// Persistence.cpp — AngelScript bindings for PersistenceBridge
//
// Exposes PersistenceBridge as the "Persistence" ref-counted type in scripts:
//
//   PlcRuntime@ rt = PlcRuntime("plant.scl");
//   Persistence@ pers = Persistence(rt);
//   pers.configure("output/", "binary", "changes_with_timestamp");
//   pers.start();
//
//   // run simulation...
//   pump.motor_speed = 1500.0;
//
//   pers.flush();   // anchor frame
//   pers.stop();    // finalise WAL
//
// =============================================================================

#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/plcsim/runtime/PersistenceBridge.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell; // PlcRuntimeWrapper
using sgrn::plcsim::persistence::PersistenceBridge;
using sgrn::plcsim::persistence::PersistenceBridgeConfig;

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper — ref-counted AS object owning a PersistenceBridge instance
// ─────────────────────────────────────────────────────────────────────────────

class PersistenceWrapper {
public:
    explicit PersistenceWrapper(PlcRuntimeWrapper* tp_rt)
        : runtime_ref_(tp_rt) {
        runtime_ref_->addRef();
        bridge_ = std::make_unique<PersistenceBridge>(tp_rt->getImpl());
    }

    ~PersistenceWrapper() {
        if (bridge_ && bridge_->isActive())
            bridge_->stop();
        runtime_ref_->release();
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    // ── Configuration ────────────────────────────────────────────────────────

    /// Configure with a single output-directory string (all other options default).
    void configure(const std::string& t_out_dir) {
        PersistenceBridgeConfig cfg;
        cfg.out_dir = t_out_dir;
        bridge_->configure(cfg);
    }

    /// Configure with out_dir, format, and archive mode.
    void configureFull(const std::string& t_out_dir, const std::string& t_format, const std::string& t_mode) {
        PersistenceBridgeConfig cfg;
        cfg.out_dir = t_out_dir;
        cfg.format = t_format;
        cfg.mode = t_mode;
        bridge_->configure(cfg);
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    void start() {
        bridge_->start();
    }
    void flush() {
        bridge_->flush();
    }
    void stop() {
        bridge_->stop();
    }

    bool isActive() const {
        return bridge_->isActive();
    }
    std::string outDir() const {
        return bridge_->outDir();
    }

private:
    PlcRuntimeWrapper* runtime_ref_{nullptr};
    std::unique_ptr<PersistenceBridge> bridge_;
    int ref_count_{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Factories
// ─────────────────────────────────────────────────────────────────────────────

/// Persistence(rt) — default config, output to current directory.
static PersistenceWrapper* Persistence_Factory(PlcRuntimeWrapper* tp_rt) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("Persistence: PlcRuntime handle must be non-null");
        return nullptr;
    }
    return new PersistenceWrapper(tp_rt);
}

/// Persistence(rt, outDir) — configure output directory immediately.
static PersistenceWrapper* Persistence_FactoryWithDir(PlcRuntimeWrapper* tp_rt, const std::string& t_out_dir) {
    auto* p_wrapper = Persistence_Factory(tp_rt);
    if (p_wrapper)
        p_wrapper->configure(t_out_dir);
    return p_wrapper;
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

Result<void, std::string> registerPersistenceTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "Persistence");
    SGRN_AS_REFCOUNTED(tp_engine, "Persistence", PersistenceWrapper);

    // Factories
    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "Persistence", asBEHAVE_FACTORY, "Persistence@ f(PlcRuntime@)", asFUNCTION(Persistence_Factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour("Persistence", asBEHAVE_FACTORY, "Persistence@ f(PlcRuntime@, const string &in)",
        asFUNCTION(Persistence_FactoryWithDir), asCALL_CDECL));

    // Configuration
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Persistence", "void configure(const string &in)",
        asMETHODPR(PersistenceWrapper, configure, (const std::string&), void), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Persistence", "void configure(const string &in, const string &in, const string &in)",
        asMETHOD(PersistenceWrapper, configureFull), asCALL_THISCALL));

    // Lifecycle
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Persistence", "void start()", asMETHOD(PersistenceWrapper, start), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Persistence", "void flush()", asMETHOD(PersistenceWrapper, flush), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("Persistence", "void stop()", asMETHOD(PersistenceWrapper, stop), asCALL_THISCALL));

    // Query
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("Persistence", "bool isActive() const", asMETHOD(PersistenceWrapper, isActive), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("Persistence", "string outDir() const", asMETHOD(PersistenceWrapper, outDir), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
