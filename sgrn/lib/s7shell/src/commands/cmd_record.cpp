// =============================================================================
// cmd_record.cpp — s7shell record sub-command
// =============================================================================

#include <sgrn/s7shell/commands/commands.hpp>
#include <sgrn/plcsim/runtime/PersistenceBridge.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>
#include <sgrn/plcsim/utils/PlcSimClock.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <cxxopts.hpp>

namespace sgrn::s7shell::commands
{

int cmd_record(int argc, char* argv[]) {
    cxxopts::Options opts("s7shell record", "Record simulation session to binary WAL");
    opts.add_options()("s,schema", "SCL schema path", cxxopts::value<std::string>())(
        "o,out", "Output directory or path", cxxopts::value<std::string>()->default_value("."))(
        "duration", "Duration in seconds", cxxopts::value<uint32_t>()->default_value("3600"))("h,help", "Print help");

    try {
        auto res = opts.parse(argc, argv);
        if (res.count("help") || !res.count("schema")) {
            fmt::print("{}\n", opts.help());
            return res.count("help") ? 0 : 1;
        }

        std::string schema_path = res["schema"].as<std::string>();
        std::string out_path = res["out"].as<std::string>();
        uint32_t duration = res["duration"].as<uint32_t>();

        auto rt = std::make_shared<::sgrn::plcsim::runtime::PlcRuntime>();
        rt->loadSclSchema(schema_path);

        ::sgrn::plcsim::persistence::PersistenceBridge bridge(rt);
        ::sgrn::plcsim::persistence::PersistenceBridgeConfig cfg;
        cfg.out_dir = out_path;
        bridge.configure(cfg);
        bridge.start();

        fmt::print(fg(fmt::color::cyan), "[record] Recording started for {} seconds...\n", duration);
        // Headless recording run
        ::sgrn::plcsim::utils::g_plc_clock.advanceMs(duration * 1000);

        bridge.flush();
        bridge.stop();
        fmt::print(fg(fmt::color::green), "[record] Recording complete: {}\n", out_path);
        return 0;
    } catch (const std::exception& e) {
        fmt::print(stderr, fg(fmt::color::red), "[record] Error: {}\n", e.what());
        return 1;
    }
}

} // namespace sgrn::s7shell::commands
