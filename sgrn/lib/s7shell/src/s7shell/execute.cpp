#include <sgrn/s7shell/S7Shell.hpp>
#include <regex>
namespace sgrn::s7shell::shell
{
static std::string rewriteReplAutoGlobal(const std::string& t_line) {
    static const std::regex kCastDecl(R"(^\s*auto\s*@?\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*cast\s*<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>\s*\()");
    static const std::regex kCtorDecl(R"(^\s*auto\s*@?\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*\()");

    std::smatch m;
    if (std::regex_search(t_line, m, kCastDecl)) {
        std::string out = t_line;
        out.replace(0, static_cast<size_t>(m.position(1)), std::string(m[2]) + "@ ");
        return out;
    }

    if (std::regex_search(t_line, m, kCtorDecl)) {
        const std::string type = m[2];
        if (type == "S7Client" || type == "PlcRuntime" || type == "GatewaySync" || type == "S7ProxySession") {
            std::string out = t_line;
            out.replace(0, static_cast<size_t>(m.position(1)), type + "@ ");
            return out;
        }
    }

    return t_line;
}

// ─────────────────────────────────────────────────────────────────────────────
// REPL execution — with 5-step fallback chain and auto-print for objects
// ─────────────────────────────────────────────────────────────────────────────
void S7Shell::execute(std::string t_line) {
    while (!t_line.empty() && std::isspace(t_line.back()))
        t_line.pop_back();
    if (!t_line.empty() && t_line.back() == ';')
        t_line.pop_back();

    if (handleMetaCommand(t_line))
        return;

    auto start_tm = std::chrono::high_resolution_clock::now();

    asIScriptModule* p_active_mod = p_script_engine_->GetModule("main");
    if (!p_active_mod)
        p_active_mod = p_repl_module_;

    // Step 1 — global variable declaration (e.g. `S7Client@ c = S7Client(...)`)
    sgrn::scripting::g_suppress_errors = true;
    const std::string global_decl = rewriteReplAutoGlobal(t_line);
    int gv = p_active_mod->CompileGlobalVar("repl", (global_decl + ";").c_str(), 0);
    sgrn::scripting::g_suppress_errors = false;
    if (gv >= 0) {
        printTiming(start_tm);
        return;
    }

    asIScriptContext* p_ctx = p_script_engine_->CreateContext();

    // Step 2 — object auto-print (toJson / toString / info / print)
    // Done BEFORE plain statement so bare variable refs like `a` are caught.
    if (tryAutoPrint(p_script_engine_, p_active_mod, p_ctx, t_line)) {
        p_ctx->Release();
        printTiming(start_tm);
        return;
    }

    // Step 3 — primitive string-concat fallback: "" + expr + "\n"
    // Covers int, float, bool, double — any type with string + opAdd.
    {
        sgrn::scripting::g_suppress_errors = true;
        std::string prim = "print(\"\" + (" + t_line + ") + \"\\n\");";
        int rp = ExecuteString(p_script_engine_, prim.c_str(), p_active_mod, p_ctx);
        sgrn::scripting::g_suppress_errors = false;
        if (rp == asEXECUTION_FINISHED) {
            p_ctx->Release();
            printTiming(start_tm);
            return;
        }
        if (rp == asEXECUTION_EXCEPTION) {
            fmt::print(stderr, fg(fmt::color::red), "Exception: {}\n", p_ctx->GetExceptionString());
            p_ctx->Release();
            printTiming(start_tm);
            return;
        }
    }

    // Step 4 — plain statement (assignment, method call, etc.)
    sgrn::scripting::g_suppress_errors = true;
    int r = ExecuteString(p_script_engine_, (t_line + ";").c_str(), p_active_mod, p_ctx);
    sgrn::scripting::g_suppress_errors = false;

    if (r == asEXECUTION_FINISHED) {
        p_ctx->Release();
        printTiming(start_tm);
        return;
    }
    if (r == asEXECUTION_EXCEPTION) {
        fmt::print(stderr, fg(fmt::color::red), "Exception: {}\n", p_ctx->GetExceptionString());
        p_ctx->Release();
        printTiming(start_tm);
        return;
    }

    // Step 5 — nothing worked: re-run to emit the compiler error to stderr
    ExecuteString(p_script_engine_, (t_line + ";").c_str(), p_active_mod, p_ctx);
    if (p_ctx->GetState() == asEXECUTION_EXCEPTION) {
        fmt::print(stderr, fg(fmt::color::red), "Exception: {}\n", p_ctx->GetExceptionString());
    }
    p_ctx->Release();
    printTiming(start_tm);
}

} // namespace sgrn::s7shell::shell
