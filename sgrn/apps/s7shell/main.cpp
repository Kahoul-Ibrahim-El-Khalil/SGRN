// ─────────────────────────────────────────────────────────────────────────────
// s7shell.cpp – S7 Automation Shell runtime entry point (Bun-like runtime)
// ─────────────────────────────────────────────────────────────────────────────
#include <fmt/format.h>
#include <sgrn/s7shell/S7Shell.hpp>
#include <sgrn/s7shell/commands/commands.hpp>
#include <cstring>
#include <cxxopts.hpp>
#include <stdexcept>

using namespace sgrn::s7shell::shell;
using namespace sgrn::s7shell::commands;

int main(int t_argc, char* t_argv[]) {
    if (t_argc > 1) {
        const char* sub = t_argv[1];

        // Linter header emitter
        if (std::strcmp(sub, "emit-as") == 0 || std::strcmp(sub, "emit-angelscript") == 0 || std::strcmp(sub, "dump-as-api") == 0 ||
            std::strcmp(sub, "as") == 0) {
            return cmd_emit_as(t_argc - 1, t_argv + 1);
        }

        // 'run' subcommand alias (e.g. `s7shell run script.as`)
        if (std::strcmp(sub, "run") == 0) {
            t_argc--;
            t_argv++;
        }
    }

    cxxopts::Options options("s7shell", "S7 Industrial Integration Runtime & Script Shell");
    options.add_options()("f,file", "AngelScript file(s) to execute (in order)", cxxopts::value<std::vector<std::string>>())(
        "s,schema", "Schema file or directory to load", cxxopts::value<std::string>())("o,output-dir",
        "Output directory for JSON schema registry if loaded",
        cxxopts::value<std::string>()->default_value(""))("h,help", "Print help")("m,man", "Print user manual & API summary");

    // Accept positional files: `s7shell script.as` or `s7shell run script.as`
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
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }

    return 0;
}
