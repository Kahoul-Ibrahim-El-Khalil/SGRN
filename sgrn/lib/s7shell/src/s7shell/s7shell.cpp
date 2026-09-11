#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/S7Shell.hpp>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/strings.hpp>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <readline/readline.h>
#include <regex>
#include <scriptbuilder/scriptbuilder.h>
#include <scripthelper/scripthelper.h>
#include <set>
#include <sstream>
#include <string_view>

namespace sgrn::s7shell::shell
{
using sgrn::utils::strings::trim;
// ─────────────────────────────────────────────────────────────────────────────
// Pretty-print JSON helper — registered as prettyJson() in AngelScript.
// Falls back to the raw string if it is not valid JSON.
// ─────────────────────────────────────────────────────────────────────────────
static std::string prettyJsonImpl(const std::string& t_compact) {
    rapidjson::Document doc;
    if (doc.Parse(t_compact.c_str()).HasParseError())
        return t_compact; // not JSON — return as-is

    rapidjson::StringBuffer buf;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buf);
    writer.SetIndent(' ', 2);
    doc.Accept(writer);
    return buf.GetString();
}

// AngelScript wrapper (value-type string semantics)
static std::string prettyJsonWrapper(const std::string& t_s) {
    return prettyJsonImpl(t_s);
}

// ─────────────────────────────────────────────────────────────────────────────

S7Shell::S7Shell()
    : AngelScriptEngine() {
    // Register pretty-print JSON helper available in all scripts and REPL
    p_script_engine_->RegisterGlobalFunction("string prettyJson(const string &in)", asFUNCTION(prettyJsonWrapper), asCALL_CDECL);

    if (auto res = registerS7Shell(p_script_engine_); res.hasError()) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] API registration failed ({}): some types may be unavailable.\n", res.error());
    }

    // Load ./angelscript.as init script (global state + helper functions)
    loadInitScript(p_script_engine_, p_repl_module_);
}

static bool isScriptPath(const std::string& t_path) {
    if (t_path.rfind("./", 0) == 0 || t_path.rfind("/", 0) == 0) {
        return true;
    }

    if (t_path.size() > 3 && t_path.compare(t_path.size() - 3, 3, ".as") == 0) {
        return true;
    }

    return std::filesystem::is_regular_file(t_path);
}

bool S7Shell::handleMetaCommand(const std::string& t_line) {
    std::string trimmed = ::sgrn::utils::strings::trim(t_line);

    if (trimmed.empty()) {
        return false;
    }

    if (trimmed.front() == '!') {
        runShellCommand(trimmed.substr(1));
        return true;
    }

    if (!isScriptPath(trimmed)) {
        return false;
    }

    if (trimmed.size() >= 2 && ((trimmed.front() == '"' && trimmed.back() == '"') || (trimmed.front() == '\'' && trimmed.back() == '\''))) {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }

    runScript(trimmed);
    return true;
}
} // namespace sgrn::s7shell::shell
