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
        // 1. Already inside a function body (or carrying one over from a
        //    prior line)? Accumulate this line FIRST, before touching depth,
        //    so the closing brace's own line is part of the buffered body.
        if (depth > 0 || !fn_buf.empty())
            fn_buf += line + '\n';
        // 2. Track brace depth for function bodies.
        for (char c : line) {
            if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
        }
        // A single stray '}' at top level must not desync the rest of the
        // file (otherwise every later function opener miscounts).
        if (depth < 0)
            depth = 0;
        // 3. The buffered function just closed (depth back to 0 on the line
        //    holding its closing brace): flush exactly once, then move on.
        //    This branch must run BEFORE any "still inside" check — the
        //    closing line itself satisfies both, and only the flush is
        //    correct for it.
        if (!fn_buf.empty() && depth == 0) {
            // Compile as function into repl_module
            sgrn::scripting::g_suppress_errors = true;
            tp_repl_mod->CompileGlobalVar("init", fn_buf.c_str(), 0);
            sgrn::scripting::g_suppress_errors = false;
            fn_buf.clear();
            continue;
        }
        // 4. Still inside a multi-line function: keep accumulating.
        if (!fn_buf.empty())
            continue;
        // 5. Top-level line that opens a function (e.g. `void foo() {`):
        //    it was not buffered in step 1 (we were not inside yet), so
        //    start the buffer here instead of compiling it as a global.
        if (depth > 0) {
            fn_buf += line + '\n';
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
    // The file must not end mid-function: a truncated/malformed script would
    // otherwise discard the buffered body silently. Warn and attempt the
    // buffered text once, matching the in-loop flush policy.
    if (!fn_buf.empty() || depth != 0) {
        fmt::print(stderr, fg(fmt::color::yellow),
            "[s7shell] Warning: init script '{}' ends inside an unterminated block "
            "(brace depth {}, {} buffered bytes); compiling buffered text anyway.\n",
            t_path, depth, fn_buf.size());
        if (!fn_buf.empty()) {
            sgrn::scripting::g_suppress_errors = true;
            tp_repl_mod->CompileGlobalVar("init", fn_buf.c_str(), 0);
            sgrn::scripting::g_suppress_errors = false;
            fn_buf.clear();
        }
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
