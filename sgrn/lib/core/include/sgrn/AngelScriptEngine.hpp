#pragma once
// =============================================================================
// AngelScriptEngine.hpp — Abstract base for interactive AngelScript REPL shells
// =============================================================================

#include <stdexcept>
#include <string>

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/FilesystemModule.hpp>
#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scripthelper/scripthelper.h>
#include <scriptstdstring/scriptstdstring.h>

#ifndef _WIN32
#include <readline/history.h>
#include <readline/readline.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sgrn::scripting
{

inline bool g_suppress_errors = false;

static void as_message_callback(const asSMessageInfo* tp_msg, void* tp_param) {
    (void)tp_param;
    const char* p_type = "ERR ";
    if (tp_msg->type == asMSGTYPE_WARNING)
        p_type = "WARN";
    else if (tp_msg->type == asMSGTYPE_INFORMATION)
        p_type = "INFO";

    if (!g_suppress_errors) {
        const char* p_section = tp_msg->section ? tp_msg->section : "";
        if (p_section[0] != '\0') {
            fmt::print(stderr, "Error: [as] {} in '{}' ({} :{}): {}\n", p_type, p_section, tp_msg->row, tp_msg->col, tp_msg->message);
        } else {
            fmt::print(stderr, "Error: [as] {} ({} :{}): {}\n", p_type, tp_msg->row, tp_msg->col, tp_msg->message);
        }
    }
}

inline void printWrapper(const std::string& t_msg) {
    fmt::print("{}", t_msg);
}

inline int randWrapper() {
    return std::rand();
}

class AngelScriptEngine {
public:
    asIScriptEngine* p_script_engine_{nullptr};
    asIScriptModule* p_repl_module_{nullptr};
    bool timing_{false};

    AngelScriptEngine() {
        p_script_engine_ = asCreateScriptEngine();
        p_script_engine_->SetMessageCallback(asFUNCTION(as_message_callback), this, asCALL_CDECL);
        p_script_engine_->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, 2);

        RegisterStdString(p_script_engine_);
        RegisterScriptArray(p_script_engine_, true);
        RegisterScriptDictionary(p_script_engine_);

        // Filesystem bindings
        ::sgrn::scripting::registerFilesystemModule(p_script_engine_);

        // Core global functions
        p_script_engine_->RegisterGlobalFunction("void print(const string &in)", asFUNCTION(printWrapper), asCALL_CDECL);
        p_script_engine_->RegisterGlobalFunction("int rand()", asFUNCTION(randWrapper), asCALL_CDECL);

        // Transposed native commands
        p_script_engine_->RegisterGlobalFunction("void sleep(int)", asMETHOD(AngelScriptEngine, sleepCmd), asCALL_THISCALL_ASGLOBAL, this);
        p_script_engine_->RegisterGlobalFunction("void clear()", asMETHOD(AngelScriptEngine, clearCmd), asCALL_THISCALL_ASGLOBAL, this);
        p_script_engine_->RegisterGlobalFunction(
            "void timing(bool)", asMETHOD(AngelScriptEngine, timingCmd), asCALL_THISCALL_ASGLOBAL, this);
        p_script_engine_->RegisterGlobalFunction("void help()", asMETHOD(AngelScriptEngine, showHelpBase), asCALL_THISCALL_ASGLOBAL, this);

        p_repl_module_ = p_script_engine_->GetModule("repl", asGM_ALWAYS_CREATE);
    }

    virtual ~AngelScriptEngine() {
        if (p_script_engine_) {
            p_script_engine_->ShutDownAndRelease();
        }
    }

    void run() {
        onStart();
#ifndef _WIN32
        std::string history_file;
        const char* p_home = std::getenv("HOME");
        if (p_home) {
            history_file = std::string(p_home) + "/.s7shell_history";
            read_history(history_file.c_str());
        }
#endif
        while (true) {
            std::string t_line = readLine();
            if (t_line.empty())
                continue;
            if (t_line == "exit" || t_line == "quit")
                break;
            execute(t_line);
            std::fflush(stdout); // ensure buffered output lands before readline() redraws the prompt
        }
#ifndef _WIN32
        if (!history_file.empty()) {
            write_history(history_file.c_str());
        }
#endif
    }
    // Command implementations
    void sleepCmd(int t_ms) {
        if (t_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(t_ms));
    }

    void clearCmd() {
        fmt::print("\033[H\033[J");
    }

    void timingCmd(bool t_on) {
        timing_ = t_on;
        fmt::print(fg(fmt::color::green), "Execution timing {}.\n", t_on ? "ENABLED" : "DISABLED");
    }

    void showHelpBase() {
        showHelp(); // Route to polymorphic virtual method
    }

protected:
    virtual void onStart() {
        fmt::print(fg(fmt::color::cyan), "AngelScript Shell\n");
    }

    virtual std::string getPrompt() const {
        return "> ";
    }

    virtual void showHelp() const {
        fmt::print(fg(fmt::color::cyan) | fmt::emphasis::bold, "\nCore Commands:\n");
        fmt::print("  help()           Show this help message\n");
        fmt::print("  sleep(ms)        Wait for N milliseconds\n");
        fmt::print("  clear()          Clear the terminal screen\n");
        fmt::print("  timing(bool)     Toggle execution time measurement\n");
        fmt::print("  print(string)    Print message to console\n");
        fmt::print("  exit / quit      Exit shell\n\n");
    }

    virtual bool handleMetaCommand(const std::string& t_line) {
        return false;
    }

    // Looks up a REPL global by name ("repl" module, then "main") and
    // renders scalar values (string, bool, all int/uint widths, float,
    // double) as text. Returns nullopt for unknown names or non-scalar
    // types (handles, arrays, objects) — callers leave those literal, so
    // text without variables passes through byte-identical.
    std::optional<std::string> replGlobalToString(const std::string& t_name) {
        if (!p_script_engine_ || t_name.empty())
            return std::nullopt;
        asIScriptModule* mods[2] = {p_repl_module_, p_script_engine_->GetModule("main")};
        for (asIScriptModule* mod : mods) {
            if (!mod)
                continue;
            const int idx = mod->GetGlobalVarIndexByName(t_name.c_str());
            if (idx < 0)
                continue;
            const char* decl = mod->GetGlobalVarDeclaration(static_cast<asUINT>(idx));
            if (!decl)
                continue;
            std::string type(decl);
            if (const auto sp = type.find(' '); sp != std::string::npos)
                type.erase(sp);
            if (type == "const")
                continue; // qualified decls are out of scope; try next module
            void* p_var = mod->GetAddressOfGlobalVar(static_cast<asUINT>(idx));
            if (!p_var)
                continue;
            if (type == "string")
                return *static_cast<std::string*>(p_var);
            if (type == "bool")
                return *static_cast<bool*>(p_var) ? "true" : "false";
            if (type == "int" || type == "int32")
                return std::to_string(*static_cast<int32_t*>(p_var));
            if (type == "int8")
                return std::to_string(static_cast<int>(*static_cast<int8_t*>(p_var)));
            if (type == "int16")
                return std::to_string(static_cast<int>(*static_cast<int16_t*>(p_var)));
            if (type == "int64")
                return std::to_string(*static_cast<int64_t*>(p_var));
            if (type == "uint" || type == "uint32")
                return std::to_string(*static_cast<uint32_t*>(p_var));
            if (type == "uint8")
                return std::to_string(static_cast<unsigned>(*static_cast<uint8_t*>(p_var)));
            if (type == "uint16")
                return std::to_string(static_cast<unsigned>(*static_cast<uint16_t*>(p_var)));
            if (type == "uint64")
                return std::to_string(*static_cast<uint64_t*>(p_var));
            if (type == "float")
                return fmt::format("{}", *static_cast<float*>(p_var));
            if (type == "double")
                return fmt::format("{}", *static_cast<double*>(p_var));
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Expands $NAME / ${NAME} from AngelScript REPL globals (see above).
    // `$$` yields a literal `$`; a `$` not followed by a name stays
    // literal; unknown names stay literal. AngelScript has no `$` syntax,
    // so previously-valid lines are unaffected. Single pass: values are
    // inserted verbatim, never re-scanned.
    std::string expandDollarVariables(const std::string& t_line) {
        if (t_line.find('$') == std::string::npos)
            return t_line;
        auto is_name_start = [](char t_c) { return std::isalpha(static_cast<unsigned char>(t_c)) || t_c == '_'; };
        auto is_name_char = [](char t_c) { return std::isalnum(static_cast<unsigned char>(t_c)) || t_c == '_'; };
        std::string out;
        out.reserve(t_line.size());
        for (size_t i = 0; i < t_line.size();) {
            if (t_line[i] != '$') {
                out += t_line[i++];
                continue;
            }
            if (i + 1 < t_line.size() && t_line[i + 1] == '$') {
                out += '$';
                i += 2;
                continue;
            }
            const size_t start = i;
            std::string name;
            if (i + 1 < t_line.size() && t_line[i + 1] == '{') {
                const size_t end = t_line.find('}', i + 2);
                if (end == std::string::npos) {
                    out += t_line[i++];
                    continue;
                }
                name = t_line.substr(i + 2, end - i - 2);
                if (name.empty() || !is_name_start(name[0]) || !std::all_of(name.begin() + 1, name.end(), is_name_char)) {
                    out += t_line[i++];
                    continue;
                }
                i = end + 1;
            } else {
                size_t k = i + 1;
                if (k >= t_line.size() || !is_name_start(t_line[k])) {
                    out += t_line[i++];
                    continue;
                }
                ++k;
                while (k < t_line.size() && is_name_char(t_line[k]))
                    ++k;
                name = t_line.substr(i + 1, k - i - 1);
                i = k;
            }
            if (const auto value = replGlobalToString(name)) {
                out += *value;
            } else {
                out += t_line.substr(start, i - start);
            }
        }
        return out;
    }

public:
    virtual void execute(std::string t_line) {
        while (!t_line.empty() && std::isspace(t_line.back()))
            t_line.pop_back();
        if (!t_line.empty() && t_line.back() == ';')
            t_line.pop_back();

        if (handleMetaCommand(t_line)) {
            return;
        }

        auto t_start_tm = std::chrono::high_resolution_clock::now();

        g_suppress_errors = true;
        int gv = p_repl_module_->CompileGlobalVar("repl", (t_line + ";").c_str(), 0);
        g_suppress_errors = false;

        if (gv >= 0) {
            printTiming(t_start_tm);
            return;
        }

        asIScriptContext* p_ctx = p_script_engine_->CreateContext();
        int r = ExecuteString(p_script_engine_, (t_line + ";").c_str(), p_repl_module_, p_ctx);

        if (r < 0) {
            g_suppress_errors = true;
            // Try evaluating as a direct print call (better for native types)
            std::string wrapped = "print(" + t_line + ");";
            int r2 = ExecuteString(p_script_engine_, wrapped.c_str(), p_repl_module_, p_ctx);
            if (r2 < 0) {
                // Fallback to concatenation for objects that support it
                wrapped = "print(\"\" + (" + t_line + ") + \"\\n\");";
                r2 = ExecuteString(p_script_engine_, wrapped.c_str(), p_repl_module_, p_ctx);
            }
            g_suppress_errors = false;
            if (r2 < 0) {
                // If expression fails, re-run statement to print the actual compiler error
                ExecuteString(p_script_engine_, (t_line + ";").c_str(), p_repl_module_, p_ctx);
            }
        }

        if (p_ctx->GetState() == asEXECUTION_EXCEPTION) {
            fmt::print(stderr, fg(fmt::color::red), "Exception: {}\n", p_ctx->GetExceptionString());
        }
        p_ctx->Release();

        printTiming(t_start_tm);
    }

private:
    std::string readLine() const {
#ifndef _WIN32
        char* p_p = readline(getPrompt().c_str());
        if (!p_p)
            return "exit";
        std::string t_line(p_p);
        if (!t_line.empty())
            add_history(p_p);
        std::free(p_p);
        return t_line;
#else
        fmt::print("{}", getPrompt());
        std::string t_line;
        if (!std::getline(std::cin, t_line))
            return "exit";
        return t_line;
#endif
    }

    void printTiming(const std::chrono::high_resolution_clock::time_point& t_start_tm) const {
        if (!timing_)
            return;
        auto end_tm = std::chrono::high_resolution_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::microseconds>(end_tm - t_start_tm);
        if (diff.count() > 1000) {
            fmt::print(fg(fmt::color::gray), "[Executed in {:.2f}ms]\n", diff.count() / 1000.0);
        } else {
            fmt::print(fg(fmt::color::gray), "[Executed in {}us]\n", diff.count());
        }
    }
};

} // namespace sgrn::scripting
