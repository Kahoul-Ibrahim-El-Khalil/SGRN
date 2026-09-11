#pragma once
// =============================================================================
// SimulationEngine.hpp — Generic headless tick-loop driver
//
// SimulationEngine knows NOTHING about field names, schema structure, or
// physics models. It is a pure time-advance engine:
//
//   1. Advance the simulated clock by timestep_ms each tick.
//   2. Call tick_fn_(t_seconds, step_index) — implemented in AngelScript.
//   3. Call markDirty() on all registered DBs so PersistenceBridge fires.
//
// Physics, field writes, and data structure knowledge live in simulation.as.
// Schema structure lives in schema.scl.
// =============================================================================

#include <sgrn/plcsim/runtime/PlcRuntime.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>

namespace sgrn::plcsim::simulation
{

/**
 * @brief Configuration parameters for the tick-loop driver.
 *
 * Physics-specific parameters (e.g. fault_scenario, noise_level) belong
 * in the AngelScript simulation script, not here. Only timing / PRNG seed
 * are engine concerns.
 */
struct SimParams {
    uint64_t seed{4219};        ///< PRNG seed (exposed to scripts via SimParams@)
    uint32_t timestep_ms{100};  ///< Simulated time step in milliseconds
    uint32_t duration_s{3600};  ///< Total simulated duration in seconds
    double noise_level{0.0};    ///< Kept for script access only; engine doesn't use it
    std::string fault_scenario; ///< Kept for script access only; engine doesn't use it
};

/**
 * @brief Tick-loop driver — calls a user-supplied callback each step.
 *
 * Usage (from AngelScript binding):
 *   engine.setTickFn([](double t_s, uint64_t step) { ... });
 *   engine.run();
 */
class SimulationEngine {
public:
    using TickFn = std::function<void(double /*t_seconds*/, uint64_t /*step_idx*/)>;

    SimulationEngine(std::shared_ptr<runtime::PlcRuntime> tsp_runtime, SimParams t_params);
    ~SimulationEngine() = default;

    /// Register the per-tick callback (called from AngelScript binding).
    void setTickFn(TickFn t_fn) {
        tick_fn_ = std::move(t_fn);
    }

    /// Run the full simulation loop.
    void run();

    // Accessors used by the AngelScript binding for SimParams@ properties.
    const SimParams& params() const {
        return params_;
    }
    SimParams& params() {
        return params_;
    }
    uint64_t currentStep() const {
        return step_count_;
    }

    /// PRNG — exposed so the tick callback can use the same seeded generator.
    double nextNormal() {
        return noise_dist_(prng_);
    }

private:
    std::shared_ptr<runtime::PlcRuntime> runtime_;
    SimParams params_;
    std::mt19937_64 prng_;
    std::normal_distribution<double> noise_dist_{0.0, 1.0};
    TickFn tick_fn_;
    uint64_t step_count_{0};
};

} // namespace sgrn::plcsim::simulation
