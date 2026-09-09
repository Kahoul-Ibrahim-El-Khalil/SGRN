// =============================================================================
// cmd_simulate.cpp — s7shell simulate sub-command
// =============================================================================

#include <sgrn/s7shell/commands/commands.hpp>
#include <sgrn/s7shell/runtime/PersistenceBridge.hpp>
#include <sgrn/s7shell/simulation/SimulationEngine.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <cxxopts.hpp>

namespace sgrn::s7shell::commands
{

int cmd_simulate(int argc, char* argv[]) {
    cxxopts::Options opts("s7shell simulate", "Deterministic synthetic data generation");
    opts.add_options()("s,schema", "SCL schema path", cxxopts::value<std::string>())(
        "seed", "PRNG seed", cxxopts::value<uint64_t>()->default_value("4219"))("duration", "Duration in seconds",
        cxxopts::value<uint32_t>()->default_value("3600"))("noise", "Noise amplitude", cxxopts::value<double>()->default_value("0.0"))(
        "fault", "Fault scenario name", cxxopts::value<std::string>()->default_value(""))(
        "o,out", "Output directory", cxxopts::value<std::string>()->default_value("."))("h,help", "Print help");

    try {
        auto res = opts.parse(argc, argv);
        if (res.count("help") || !res.count("schema")) {
            fmt::print("{}\n", opts.help());
            return res.count("help") ? 0 : 1;
        }

        auto rt = std::make_shared<runtime::PlcRuntime>();
        rt->loadSclSchema(res["schema"].as<std::string>());

        simulation::SimParams p;
        p.seed = res["seed"].as<uint64_t>();
        p.duration_s = res["duration"].as<uint32_t>();
        p.noise_level = res["noise"].as<double>();
        p.fault_scenario = res["fault"].as<std::string>();

        persistence::PersistenceBridge bridge(rt);
        persistence::PersistenceBridgeConfig cfg;
        cfg.out_dir = res["out"].as<std::string>();
        bridge.configure(cfg);
        bridge.start();

        simulation::SimulationEngine sim(rt, p);
        sim.run();

        bridge.flush();
        bridge.stop();
        return 0;
    } catch (const std::exception& e) {
        fmt::print(stderr, fg(fmt::color::red), "[simulate] Error: {}\n", e.what());
        return 1;
    }
}

} // namespace sgrn::s7shell::commands
