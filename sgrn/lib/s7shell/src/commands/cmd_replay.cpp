// =============================================================================
// cmd_replay.cpp — s7shell replay sub-command
// =============================================================================

#include <sgrn/plcsim/replay/WalReplayer.hpp>
#include <sgrn/s7shell/commands/commands.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <cxxopts.hpp>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::commands
{

int cmd_replay(int argc, char* argv[]) {
    cxxopts::Options opts("s7shell replay", "Replay WAL archive into digital twin");
    opts.add_options()("archive", "Path to .bin.zst or .jsonl.zst archive", cxxopts::value<std::string>())(
        "speed", "Playback speed multiplier", cxxopts::value<double>()->default_value("1.0"))("h,help", "Print help");

    opts.parse_positional({"archive"});

    try {
        auto res = opts.parse(argc, argv);
        if (res.count("help") || !res.count("archive")) {
            fmt::print("{}\n", opts.help());
            return res.count("help") ? 0 : 1;
        }

        std::string archive = res["archive"].as<std::string>();
        double speed = res["speed"].as<double>();

        ::sgrn::plcsim::replay::WalReplayer replayer(archive);
        replayer.speed(speed);
        return replayer.run() ? 0 : 1;
    } catch (const std::exception& e) {
        fmt::print(stderr, fg(fmt::color::red), "[replay] Error: {}\n", e.what());
        return 1;
    }
}

} // namespace sgrn::s7shell::commands
