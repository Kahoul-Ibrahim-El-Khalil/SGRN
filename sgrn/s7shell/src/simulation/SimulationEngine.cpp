// =============================================================================
// SimulationEngine.cpp — Generic headless tick-loop driver
//
// Physics are defined in AngelScript (simulation.as) — not here.
// This file contains only timing, clock advance, PRNG, and markDirty logic.
// =============================================================================

#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/s7shell/simulation/SimulationEngine.hpp>
#include <sgrn/s7shell/utils/PlcSimClock.hpp>

#include <fmt/color.h>
#include <fmt/format.h>

namespace sgrn::s7shell::simulation
{

SimulationEngine::SimulationEngine(std::shared_ptr<runtime::PlcRuntime> tsp_runtime, SimParams t_params)
    : runtime_(std::move(tsp_runtime))
    , params_(std::move(t_params))
    , prng_(params_.seed) {
}

void SimulationEngine::run() {
    if (!runtime_) {
        fmt::print(stderr, fg(fmt::color::red), "[SimulationEngine] run() called with null runtime\n");
        return;
    }

    const uint64_t total_steps = (static_cast<uint64_t>(params_.duration_s) * 1000) / params_.timestep_ms;

    fmt::print(fg(fmt::color::cyan), "[SimulationEngine] Starting: seed={}, duration={}s, timestep={}ms, steps={}\n", params_.seed,
        params_.duration_s, params_.timestep_ms, total_steps);

    if (!tick_fn_) {
        fmt::print(stderr, fg(fmt::color::yellow),
            "[SimulationEngine] No tick callback registered — call sim.onTick(@yourFn) before sim.run().\n");
    }

    for (uint64_t i = 0; i < total_steps; ++i) {
        // 1. Compute simulated time in seconds for this tick.
        const double t_s = static_cast<double>(i) * params_.timestep_ms * 0.001;

        // 2. Invoke the AngelScript tick callback — it writes fields into PlcMemory
        //    via = operator proxies (ScriptFieldProxy), which enqueue PlcCommands.
        if (tick_fn_)
            tick_fn_(t_s, i);

        // 3. Flush the enqueued writes to actual memory BEFORE markDirty fires.
        //    Without this, PersistenceBridge::makeDbReader() reads stale bytes
        //    because the command processor hasn't applied the proxy writes yet.
        runtime_->getMemory().processor()->processCommands();

        // 4. Signal dirty on all registered DBs so PersistenceBridge captures data.
        //    Iterates schema.dbs() — engine never hard-codes any DB numbers.
        for (const auto& [db_num, db_schema] : runtime_->getSchema().dbs()) {
            if (db_schema.size_bytes > 0)
                runtime_->markDirty(db_num, 0, static_cast<uint32_t>(db_schema.size_bytes));
        }

        // 4. Advance the simulated clock (so PersistenceBridge timestamps are
        //    spaced by timestep_ms — not clustered at the same real-time instant).
        ::sgrn::s7shell::shell::g_plc_clock.advanceMs(static_cast<int64_t>(params_.timestep_ms));

        ++step_count_;
    }

    fmt::print(fg(fmt::color::green), "[SimulationEngine] Finished. Total steps: {}\n", step_count_);
}

} // namespace sgrn::s7shell::simulation
