// ─────────────────────────────────────────────────────────────────────────────
// s7shell.cpp – S7 Shell entry point
// ─────────────────────────────────────────────────────────────────────────────
#include <fmt/format.h>
#include <sgrn/s7shell/S7Shell.hpp>
#include <cxxopts.hpp>
#include <stdexcept>

using namespace sgrn::s7shell::shell;

int main(int t_argc, char* t_argv[]) {
    cxxopts::Options options("s7shell", "S7 Programmable Automation Shell");
    options.add_options()("f,file", "AngelScript file(s) to execute (in order)", cxxopts::value<std::vector<std::string>>())(
        "s,schema", "Schema file or directory to load", cxxopts::value<std::string>())(
        "o,output-dir", "Output directory for JSON schema registry if loaded", cxxopts::value<std::string>()->default_value(""))(
        "h,help", "Print help")("m,man", "Declare how to use its API (man page / user manual)");

    // Accept any number of positional files
    options.parse_positional({"file"});

    try {
        auto result = options.parse(t_argc, t_argv);

        if (result.count("help")) {
            fmt::print("{}\n", options.help());
            return 0;
        }

        if (result.count("man")) {
            S7Shell shell;
            shell.printHelp();
            return 0;
        }

        S7Shell shell;

        if (result.count("schema")) {
            shell.loadSchema(result["schema"].as<std::string>(), result["output-dir"].as<std::string>());
        }

        if (result.count("file")) {
            const auto& files = result["file"].as<std::vector<std::string>>();
            if (files.size() == 1)
                shell.runScript(files[0]);
            else
                shell.runScripts(files);
        } else {
            shell.run();
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "SchemaError: {}\n", e.what());
        return 1;
    }

    return 0;
}
