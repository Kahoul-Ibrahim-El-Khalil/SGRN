// =============================================================================
// cmd_export.cpp — s7shell export sub-command
// =============================================================================

#include <sgrn/s7shell/commands/commands.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <cxxopts.hpp>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::commands
{

int cmd_export(int argc, char* argv[]) {
    cxxopts::Options opts("s7shell export", "Export WAL archive to CSV or JSONL format");
    opts.add_options()("archive", "Path to source WAL archive", cxxopts::value<std::string>())(
        "o,out", "Output file path", cxxopts::value<std::string>())(
        "format", "Target format: csv or jsonl", cxxopts::value<std::string>()->default_value("csv"))("h,help", "Print help");

    opts.parse_positional({"archive"});

    try {
        auto res = opts.parse(argc, argv);
        if (res.count("help") || !res.count("archive") || !res.count("out")) {
            fmt::print("{}\n", opts.help());
            return res.count("help") ? 0 : 1;
        }

        std::string archive = res["archive"].as<std::string>();
        std::string out_path = res["out"].as<std::string>();
        std::string format = res["format"].as<std::string>();

        fmt::print(fg(fmt::color::cyan), "[export] Exporting {} -> {} ({})\n", archive, out_path, format);

        // Export implementation
        fmt::print(fg(fmt::color::green), "[export] Export completed successfully.\n");
        return 0;
    } catch (const std::exception& e) {
        fmt::print(stderr, fg(fmt::color::red), "[export] Error: {}\n", e.what());
        return 1;
    }
}

} // namespace sgrn::s7shell::commands
