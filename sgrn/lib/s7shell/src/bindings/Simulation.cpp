// =============================================================================
// Simulation.cpp — AngelScript bindings for SimulationEngine / SimParams
//
// Exposed AS API:
//
//   funcdef void SimTickFn(PlcRuntime@ rt, double t_s, uint64 step_idx);
//
//   SimParams@ p = SimParams();
//   p.seed        = 88419;
//   p.duration_s  = 1800;
//   p.timestep_ms = 100;
//
//   SimEngine@ sim = SimEngine(rt, p);
//   sim.onTick(@myTick);   // register physics callback
//   sim.run();             // drives the loop — calls myTick() each step
//
// The C++ engine is a pure tick-loop driver. Physics live in the script.
// =============================================================================

#include <sgrn/plcsim/simulation/SimulationEngine.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell; // PlcRuntimeWrapper
using sgrn::plcsim::simulation::SimParams;
using sgrn::plcsim::simulation::SimulationEngine;

// ─────────────────────────────────────────────────────────────────────────────
// SimParamsWrapper — ref-counted AS handle for SimParams
// ─────────────────────────────────────────────────────────────────────────────

class SimParamsWrapper {
public:
    SimParams& get() {
        return params_;
    }
    const SimParams& get() const {
        return params_;
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

private:
    // All data members share one access block so the class stays
    // standard-layout: RegisterObjectProperty() below locates properties
    // with offsetof(), which requires it. registerSimulationTypes is a
    // friend so it may name params_ for those offsets.
    friend Result<void, std::string> registerSimulationTypes(asIScriptEngine* tp_engine);
    SimParams params_;
    int ref_count_{1};
};

static SimParamsWrapper* SimParams_Factory() {
    return new SimParamsWrapper();
}

// ─────────────────────────────────────────────────────────────────────────────
// SimEngineWrapper — ref-counted AS handle for SimulationEngine
// ─────────────────────────────────────────────────────────────────────────────

class SimEngineWrapper {
public:
    SimEngineWrapper(PlcRuntimeWrapper* tp_rt, SimParamsWrapper* tp_params, asIScriptEngine* tp_as_engine)
        : rt_ref_(tp_rt)
        , as_engine_(tp_as_engine) {
        tp_rt->addRef();
        engine_ = std::make_unique<SimulationEngine>(tp_rt->getImpl(), tp_params ? tp_params->get() : SimParams{});
    }

    ~SimEngineWrapper() {
        clearTickFn();
        if (rt_ref_)
            rt_ref_->release();
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    // ── onTick — register the AngelScript tick callback ───────────────────────
    // Signature: void SimTickFn(PlcRuntime@ rt, double t_s, uint64 step_idx)
    void onTick(asIScriptFunction* t_fn) {
        clearTickFn();
        if (!t_fn)
            return;

        tick_fn_as_ = t_fn;
        // Create a persistent context reused across all ticks (avoids 18,000 allocs).
        tick_ctx_ = as_engine_->CreateContext();

        // Build a C++ lambda that the engine calls each step.
        PlcRuntimeWrapper* rt = rt_ref_;
        engine_->setTickFn([this, rt](double t_s, uint64_t step_idx) { callAsTick(rt, t_s, step_idx); });
    }

    // ── run — start the loop ─────────────────────────────────────────────────
    void run() {
        if (engine_)
            engine_->run();
    }

    // ── nextNormal — expose PRNG so scripts can sample noise ─────────────────
    double nextNormal() {
        return engine_ ? engine_->nextNormal() : 0.0;
    }

private:
    // Call the registered AS tick function for one step.
    void callAsTick(PlcRuntimeWrapper* tp_rt, double t_s, uint64_t step_idx) {
        if (!tick_ctx_ || !tick_fn_as_)
            return;

        // Resolve funcdef delegate if needed.
        asIScriptFunction* actual_fn = tick_fn_as_;
        if (tick_fn_as_->GetFuncType() == asFUNC_DELEGATE)
            actual_fn = tick_fn_as_->GetDelegateFunction();

        if (tick_ctx_->Prepare(actual_fn) < 0)
            return;

        tick_ctx_->SetArgObject(0, tp_rt);                         // PlcRuntime@ rt
        tick_ctx_->SetArgDouble(1, t_s);                           // double t_s
        tick_ctx_->SetArgQWord(2, static_cast<asQWORD>(step_idx)); // uint64 step_idx

        const int r = tick_ctx_->Execute();
        if (r == asEXECUTION_EXCEPTION) {
            int col = 0;
            const int line = tick_ctx_->GetExceptionLineNumber(&col);
            const char* fn = "?";
            if (auto* ef = tick_ctx_->GetExceptionFunction())
                fn = ef->GetName();
            fmt::print(stderr, fg(fmt::color::red), "[SimEngine] Exception in tick callback at {}:{} — {}: {}\n", fn, line, col,
                tick_ctx_->GetExceptionString());
        }
    }

    void clearTickFn() {
        if (tick_ctx_) {
            tick_ctx_->Release();
            tick_ctx_ = nullptr;
        }
        if (tick_fn_as_) {
            tick_fn_as_->Release();
            tick_fn_as_ = nullptr;
        }
        engine_->setTickFn({});
    }

    PlcRuntimeWrapper* rt_ref_{nullptr};
    asIScriptEngine* as_engine_{nullptr};
    asIScriptContext* tick_ctx_{nullptr};
    asIScriptFunction* tick_fn_as_{nullptr};
    std::unique_ptr<SimulationEngine> engine_;
    int ref_count_{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Factories — need access to the AS engine to create contexts
// ─────────────────────────────────────────────────────────────────────────────

static SimEngineWrapper* SimEngine_Factory(PlcRuntimeWrapper* tp_rt, SimParamsWrapper* tp_params) {
    if (!tp_rt || !tp_rt->getImpl()) {
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("SimEngine: PlcRuntime handle must be non-null");
        return nullptr;
    }
    asIScriptEngine* as_engine = nullptr;
    if (auto* p_ctx = asGetActiveContext())
        as_engine = p_ctx->GetEngine();
    return new SimEngineWrapper(tp_rt, tp_params, as_engine);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

Result<void, std::string> registerSimulationTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    // ── SimParams ─────────────────────────────────────────────────────────────
    SGRN_AS_TYPE(tp_engine, "SimParams");
    SGRN_AS_REFCOUNTED(tp_engine, "SimParams", SimParamsWrapper);

    SGRN_AS_REG(
        tp_engine->RegisterObjectBehaviour("SimParams", asBEHAVE_FACTORY, "SimParams@ f()", asFUNCTION(SimParams_Factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectProperty("SimParams", "uint64 seed", offsetof(SimParamsWrapper, params_.seed)));
    SGRN_AS_REG(tp_engine->RegisterObjectProperty("SimParams", "uint timestep_ms", offsetof(SimParamsWrapper, params_.timestep_ms)));
    SGRN_AS_REG(tp_engine->RegisterObjectProperty("SimParams", "uint duration_s", offsetof(SimParamsWrapper, params_.duration_s)));
    SGRN_AS_REG(tp_engine->RegisterObjectProperty("SimParams", "double noise_level", offsetof(SimParamsWrapper, params_.noise_level)));
    SGRN_AS_REG(tp_engine->RegisterObjectProperty("SimParams", "string fault", offsetof(SimParamsWrapper, params_.fault_scenario)));

    // ── funcdef — the tick callback signature ────────────────────────────────
    // void SimTickFn(PlcRuntime@ rt, double t_s, uint64 step_idx)
    SGRN_AS_REG(tp_engine->RegisterFuncdef("void SimTickFn(PlcRuntime@, double, uint64)"));

    // ── SimEngine ─────────────────────────────────────────────────────────────
    SGRN_AS_TYPE(tp_engine, "SimEngine");
    SGRN_AS_REFCOUNTED(tp_engine, "SimEngine", SimEngineWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "SimEngine", asBEHAVE_FACTORY, "SimEngine@ f(PlcRuntime@, SimParams@)", asFUNCTION(SimEngine_Factory), asCALL_CDECL));

    // onTick — register the AS tick callback before calling run().
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("SimEngine", "void onTick(SimTickFn@)", asMETHOD(SimEngineWrapper, onTick), asCALL_THISCALL));

    // run — execute the full simulation loop.
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("SimEngine", "void run()", asMETHOD(SimEngineWrapper, run), asCALL_THISCALL));

    // nextNormal — sample from the seeded PRNG (for noise in script physics).
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("SimEngine", "double nextNormal()", asMETHOD(SimEngineWrapper, nextNormal), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
