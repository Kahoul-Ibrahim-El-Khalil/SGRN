#include <sgrn/s7shell/S7Shell.hpp>
#include <scriptbuilder/scriptbuilder.h>
#include <scripthelper/scripthelper.h>

namespace sgrn::s7shell::shell
{
// ─────────────────────────────────────────────────────────────────────────────
// Load ./angelscript.as (or user-specified path) as init script.
// Global variable declarations go into the persistent repl_module;
// free functions are compiled into a "init_script" module and stay registered.
// ─────────────────────────────────────────────────────────────────────────────
void loadInitScript(asIScriptEngine* tp_engine, asIScriptModule* tp_repl_mod, const std::string& t_path) {
    if (!std::filesystem::exists(t_path))
        return;

    std::ifstream ifs(t_path);
    if (!ifs.is_open())
        return;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    fmt::print(fg(fmt::color::cyan), "[s7shell] Loading init script: {}\n", t_path);

    // Step 1 — compile each top-level statement into repl_module (global vars)
    // Split on ; and try each chunk as a global declaration.
    // Lines starting with // are comments; function bodies (containing {}) go
    // to the function module instead.
    std::istringstream ss(content);
    std::string line;
    std::string fn_buf; // accumulate function bodies
    int depth = 0;
    while (std::getline(ss, line)) {
        // strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // Track brace depth for function bodies
        for (char c : line) {
            if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
        }
        if (depth > 0 || (!fn_buf.empty() && depth == 0)) {
            fn_buf += line + '\n';
            continue;
        }
        // Flush completed function
        if (!fn_buf.empty()) {
            fn_buf += line + '\n';
            // Compile as function into repl_module
            sgrn::scripting::g_suppress_errors = true;
            tp_repl_mod->CompileGlobalVar("init", fn_buf.c_str(), 0);
            sgrn::scripting::g_suppress_errors = false;
            fn_buf.clear();
            continue;
        }
        // Plain statement / global var
        std::string trimmed = line;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
            trimmed.erase(trimmed.begin());
        if (trimmed.empty() || trimmed.rfind("//", 0) == 0)
            continue;
        if (trimmed.back() != ';')
            trimmed += ';';
        sgrn::scripting::g_suppress_errors = true;
        tp_repl_mod->CompileGlobalVar("init", trimmed.c_str(), 0);
        sgrn::scripting::g_suppress_errors = false;
    }
    // Step 2 — also compile the whole file as a module so functions are callable
    CScriptBuilder builder;
    if (builder.StartNewModule(tp_engine, "init_script") >= 0) {
        builder.AddSectionFromMemory("angelscript.as", content.c_str());
        sgrn::scripting::g_suppress_errors = true;
        builder.BuildModule();
        sgrn::scripting::g_suppress_errors = false;
    }
    fmt::print(fg(fmt::color::cyan), "[s7shell] Init script loaded.\n");
}

} // namespace sgrn::s7shell::shell
