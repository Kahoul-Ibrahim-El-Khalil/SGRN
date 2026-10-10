#include <sgrn/s7shell/S7Shell.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/schema/schema.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/strings.hpp>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
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
#include <stdexcept>
#include <string>
#include <string_view>

namespace sgrn::s7shell::shell
{

// ─────────────────────────────────────────────────────────────────────────────
// Inject DataBlock@ globals into the REPL module for every registered DB.
// Each DB becomes an AS global:  DataBlock@ primary_coolant = cast<DataBlock>(plc.db(11));
// Called after any schema load so the REPL immediately sees the new symbols.
//
// NOTE: We iterate the schema store attached to the SchemaVMRegistry, not a
// specific S7Client, because at schema-load time there may be no live PLC
// connection. The actual DataBlock handle is resolved lazily through plc.db().
// The caller must have a variable named `plc` of type S7Client@ in the module
// for the cast to resolve. If not yet defined, the CompileGlobalVar simply
// silently fails (g_suppress_errors guards the output).
// ─────────────────────────────────────────────────────────────────────────────
void injectDbRefs(
    asIScriptEngine* tp_engine, asIScriptModule* tp_mod, const sgrn::scl::PlcSchemaStore& t_store, const std::string& t_client_var) {
    const auto& dbs = t_store.dbs();
    if (dbs.empty())
        return;

    fmt::print(fg(fmt::color::gray), "[s7shell] Injecting {} DataBlock reference(s) into REPL:\n", dbs.size());
    for (const auto& [num, db] : dbs) {
        const std::string name =
            sgrn::utils::strings::sanitizeIdentifier(db.db_name.empty() ? fmt::format("DB{}", db.db_number) : db.db_name);
        const std::string as_type = "Db_" + name;

        // Bare name matching exact SCL schema declaration (e.g. MotorCommand, Supervisor)
        const std::string prefix = (t_client_var == "plc") ? "" : t_client_var + "_";

        // Bare name (always injected)
        const std::string stmt_bare =
            fmt::format("{}@ get_{}() {{ return cast<{}>({}.db({})); }}", as_type, name, as_type, t_client_var, db.db_number);
        // Prefixed name (for multi-client REPL)
        const std::string stmt_pfx = prefix.empty() ? ""
                                                    : fmt::format("{}@ get_{}{}() {{ return cast<{}>({}.db({})); }}", as_type, prefix, name,
                                                          as_type, t_client_var, db.db_number);
        // Generic db-number accessor
        const std::string stmt_num =
            fmt::format("{}@ get_db{}() {{ return cast<{}>({}.db({})); }}", as_type, db.db_number, as_type, t_client_var, db.db_number);

        sgrn::scripting::g_suppress_errors = true;
        int r = tp_mod->CompileGlobalVar("inject", stmt_bare.c_str(), 0);
        if (!stmt_pfx.empty())
            tp_mod->CompileGlobalVar("inject", stmt_pfx.c_str(), 0);
        tp_mod->CompileGlobalVar("inject", stmt_num.c_str(), 0);
        sgrn::scripting::g_suppress_errors = false;

        if (r >= 0)
            fmt::print(fg(fmt::color::gray), "  \u2713 DataBlock@ {} (DB{})\n", name, db.db_number);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Builds the auto-generated DB preamble for runScript() compilation.
// Returns AS source that declares DataBlock@ handles for all schema DBs.
// ─────────────────────────────────────────────────────────────────────────────
std::string buildDbPreamble(const sgrn::scl::PlcSchemaStore& t_store, const std::string& t_client_var) {
    std::string out;
    const std::string prefix = (t_client_var == "plc") ? "" : t_client_var + "_";
    for (const auto& [num, db] : t_store.dbs()) {
        const std::string name =
            sgrn::utils::strings::sanitizeIdentifier(db.db_name.empty() ? fmt::format("DB{}", db.db_number) : db.db_name);
        const std::string as_type = "Db_" + name;

        // Bare name — primary API matching SCL schema (MotorCommand.start = true;)
        out += fmt::format("{}@ {} {{ get {{ return ({} !is null) ? cast<{}>({}.db({})) : null; }} }}\n", as_type, name, t_client_var,
            as_type, t_client_var, db.db_number);

        // Prefixed name — multi-client disambiguation (g_rt_MotorCommand, etc.)
        if (!prefix.empty()) {
            out += fmt::format("{}@ {}{} {{ get {{ return ({} !is null) ? cast<{}>({}.db({})) : null; }} }}\n", as_type, prefix, name,
                t_client_var, as_type, t_client_var, db.db_number);
        }

        // Generic db-number accessor — db1, db2, ...
        out += fmt::format("{}@ db{} {{ get {{ return ({} !is null) ? cast<{}>({}.db({})) : null; }} }}\n", as_type, db.db_number,
            t_client_var, as_type, t_client_var, db.db_number);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Script runner — structured error cascade with function/line/column
// ─────────────────────────────────────────────────────────────────────────────
void S7Shell::loadSchema(const std::string& t_schema_path, const std::string& t_output_dir) {
    std::filesystem::path p(t_schema_path);
    std::string expanded = sgrn::utils::filesystem::expandUserPath(p.string());
    std::filesystem::path resolved_path(expanded);
    if (resolved_path.is_relative()) {
        resolved_path = std::filesystem::absolute(resolved_path);
    }
    std::string resolved = resolved_path.lexically_normal().string();

    // Deduplicate: skip if already loaded
    if (loaded_schemas_.count(resolved)) {
        fmt::print(fg(fmt::color::gray), "[s7shell] Schema already loaded, skipping: {}\n", resolved);
        return;
    }

    if (std::filesystem::exists(resolved)) {
        sgrn::scl::PlcSchemaStore t_store;
        std::optional<sgrn::scl::SclError> err;

        if (resolved.ends_with(".json")) {
            auto res = t_store.loadFromJsonFile(resolved);
            if (res.hasError())
                err = res.error();
        } else if (resolved.ends_with(".scl") || resolved.ends_with(".xml")) {
            auto res = t_store.loadFile(resolved);
            if (res.hasError())
                err = res.error();
        } else {
            auto res = t_store.loadSchema(resolved); // this handles directories
            if (res.hasError())
                err = res.error();
        }

        if (err.has_value()) {
            fmt::print(stderr, fg(fmt::color::red), "[s7shell] Schema compilation failed for '{}': {}\n", resolved, toString(*err));
            // Surface any partial warnings gathered during the attempted load
            for (const auto& w : t_store.warnings())
                fmt::print(stderr, fg(fmt::color::yellow), "[s7shell]   warning: {}\n", w);
            return;
        }

        loaded_schemas_.insert(resolved);

        sgrn::scripting::ScriptHost host(p_script_engine_);
        registerSchemaTypes(host, t_store);
        injectDbRefs(p_script_engine_, p_repl_module_, t_store, "plc");
        db_preamble_ += buildDbPreamble(t_store, "plc");

        if (!t_output_dir.empty()) {
            std::filesystem::create_directories(t_output_dir);
            std::string json = t_store.toJson(std::nullopt, false, true);
            std::filesystem::path out_file = std::filesystem::path(t_output_dir) / "registry.json";
            sgrn::utils::filesystem::writeStringToFile(out_file.string(), json);
            fmt::print(fg(fmt::color::green), "[s7shell] Wrote compiled schema JSON to {}\n", out_file.string());
        }
    } else {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Schema path does not exist: {}\n", resolved);
    }
}
} // namespace sgrn::s7shell::shell
