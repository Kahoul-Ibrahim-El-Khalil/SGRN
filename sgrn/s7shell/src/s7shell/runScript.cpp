#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/Result.hpp>
#include <sgrn/s7shell/S7Shell.hpp>
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
#include <string>
#include <string_view>

namespace sgrn::s7shell::shell
{

namespace fs = std::filesystem;

using sgrn::Result;
using ::sgrn::scl::SclError;
using sgrn::utils::filesystem::expandUserPath;
// Forward declaration — scanIncludes() recurses back into preScanFile() for
// each resolved #include/import target.
static void preScanFile(const std::string& t_filename, const std::string& t_content, asIScriptEngine* tp_script_engine,
    asIScriptModule* tp_repl_module, std::string& t_db_preamble, std::set<std::string>& t_scanned_files,
    std::set<std::string>& t_loaded_schemas);

// ─────────────────────────────────────────────────────────────────────────────
// resolveAgainstBase — resolve a possibly-relative, possibly-~-prefixed path
// against a base directory (the scanning file's own directory).
//
// IMPORTANT: tilde expansion happens BEFORE the relative-path join. Doing it
// after (the previous behavior) corrupts a leading "~" into a literal mid-path
// directory once it's been joined onto base_dir — e.g. "~/schema.scl" joined
// onto ".../pump" becomes ".../pump/~/schema.scl", which no longer starts
// with "~" and so never expands. Expanding first avoids that entirely.
// ─────────────────────────────────────────────────────────────────────────────
static std::string resolveAgainstBase(const fs::path& t_base_dir, const std::string& t_raw_path) {
    const std::string expanded = expandUserPath(t_raw_path);
    fs::path p(expanded);
    if (p.is_relative())
        p = t_base_dir / p;
    return p.lexically_normal().string();
}

// ─────────────────────────────────────────────────────────────────────────────
// extractLiteralOrConstArg — resolve a captured call argument token into its
// actual string value. Handles three forms:
//   1. A quoted literal:            "path/or/content"  |  'path/or/content'
//   2. An AngelScript heredoc:      """multi-line, quote-safe content"""
//      (needed for inline SCL, which is full of DATA_BLOCK "Name"-style
//      double quotes that would otherwise terminate a normal literal early)
//   3. An identifier referring to a `const string NAME = <literal>;`
//      declared elsewhere in the same file (either quoted or heredoc form)
// ─────────────────────────────────────────────────────────────────────────────
static Result<std::string, std::string> extractLiteralOrConstArg(const std::string& t_content, const std::string& t_arg) {
    // Try heredoc first (greedy across newlines), then quoted.
    try {
        // Form 2: the argument itself is a heredoc literal
        if (t_arg.size() >= 6 && t_arg.compare(0, 3, "\"\"\"") == 0 && t_arg.compare(t_arg.size() - 3, 3, "\"\"\"") == 0)
            return t_arg.substr(3, t_arg.size() - 6);

        // Form 1: the argument itself is a quoted literal
        if (t_arg.size() >= 2) {
            const char quote = t_arg.front();
            if ((quote == '"' || quote == '\'') && t_arg.back() == quote)
                return t_arg.substr(1, t_arg.size() - 2);
        }

        // Form 3: the argument is an identifier — look up its declaration.
        std::regex heredoc_var_regex(R"((?:const\s+)?string\s+)" + t_arg + R"RX(\s*=\s*"""([\s\S]*?)"""\s*;)RX");
        std::smatch heredoc_match;
        if (std::regex_search(t_content, heredoc_match, heredoc_var_regex))
            return heredoc_match[1].str();
        std::regex var_regex(R"((?:const\s+)?string\s+)" + t_arg + R"(\s*=\s*["']([^"']*)["']\s*;)");
        std::smatch var_match;
        if (std::regex_search(t_content, var_match, var_regex))
            return var_match[1].str();
    } catch (const std::exception& e) {
        return Result<std::string, std::string>::Error(e.what());
    }
    return "";
}
// ─────────────────────────────────────────────────────────────────────────────
// SchemaLoadResult — holds loaded schema and its deduplication key
// ─────────────────────────────────────────────────────────────────────────────
struct SchemaLoadResult {
    ::sgrn::scl::PlcSchemaStore store;
    std::string schema_key;
};

// ─────────────────────────────────────────────────────────────────────────────
// loadScannedSchema — load a PlcSchemaStore for a detected schema-load call.
//
// Mirrors PlcSchemaStore::loadSchema()'s own path-vs-content auto-detection,
// but resolves the "is this a file?" check script-relative and ~-aware first
// (resolveAgainstBase), and — critically — if it's NOT a file, falls back to
// the RAW, unresolved argument as inline content, not the resolved/joined
// path. `resolved` only means something for real filesystem paths; joining
// it onto a directory and lexically-normalizing it is meaningless (and
// potentially corrupting) for literal SCL source text.
// ─────────────────────────────────────────────────────────────────────────────
static Result<SchemaLoadResult, SclError> loadScannedSchema(const std::string& t_method, const std::string& t_raw_arg,
    const fs::path& t_script_dir, const std::set<std::string>& t_loaded_schemas) {
    const auto resolved = resolveAgainstBase(t_script_dir, t_raw_arg);
    std::string schema_key = resolved; // Use resolved path as deduplication key

    // Check for duplicate BEFORE loading
    if (t_loaded_schemas.count(schema_key)) {
        return SclError::DuplicateDefinition;
    }

    if (fs::exists(resolved)) {
        if (t_method == "loadJsonSchema") {
            auto res = ::sgrn::scl::PlcSchemaStore::loadFromJsonFile(resolved);
            if (res.hasError())
                return res.error();
            return SchemaLoadResult{std::move(res.value()), schema_key};
        }

        ::sgrn::scl::PlcSchemaStore store;
        auto res = store.loadFile(resolved);
        if (res.hasError())
            return res.error();
        return SchemaLoadResult{std::move(store), schema_key};
    }

    // Not a file on disk — treat the raw argument as inline schema source.
    // Only SCL-flavored calls (loadSclSchema/loadSchema) support this;
    // loadJsonSchema stays file-only.
    if (t_method == "loadJsonSchema")
        return SclError::NotFound;

    ::sgrn::scl::PlcSchemaStore store;
    auto res = store.loadSchema(t_raw_arg); // raw arg, not `resolved`
    if (res.hasError()) {
        return res.error();
    }
    // For inline schemas, use a hash of the content as key
    schema_key = "inline:" + std::to_string(std::hash<std::string>{}(t_raw_arg));

    // Check again for inline schemas (in case same inline schema loaded twice)
    if (t_loaded_schemas.count(schema_key)) {
        return SclError::DuplicateDefinition;
    }

    return SchemaLoadResult{std::move(store), schema_key};
}

// ─────────────────────────────────────────────────────────────────────────────
// registerScannedSchema — wire a loaded schema into the AngelScript engine:
// register its types, inject the DataBlock@ globals for `client_var`, and
// accumulate its DB preamble for later module compilation.
// ─────────────────────────────────────────────────────────────────────────────
static void registerScannedSchema(asIScriptEngine* tp_engine, asIScriptModule* tp_module, const ::sgrn::scl::PlcSchemaStore& t_store,
    const std::string& t_client_var, std::string& t_db_preamble) {
    sgrn::scripting::ScriptHost host(tp_engine);
    registerSchemaTypes(host, t_store);
    injectDbRefs(tp_engine, tp_module, t_store, t_client_var);
    t_db_preamble += buildDbPreamble(t_store, t_client_var);
}

// ─────────────────────────────────────────────────────────────────────────────
// scanIncludes — find `import "x.as";` / `#include "x.as"` lines and
// recursively preScanFile() each resolved target.
// ─────────────────────────────────────────────────────────────────────────────
static void scanIncludes(const std::string& t_content, const std::string& t_abs_path, asIScriptEngine* tp_script_engine,
    asIScriptModule* tp_repl_module, std::string& t_db_preamble, std::set<std::string>& t_scanned_files,
    std::set<std::string>& t_loaded_schemas) {
    static const std::regex include_regex(
        R"((?:^\s*import\s+["']([^"']+)["']\s*;?)|(?:^\s*#include\s+["']([^"']+)["']))", std::regex::multiline);

    const fs::path script_dir = std::filesystem::path(t_abs_path).parent_path();

    std::smatch inc_match;
    std::string::const_iterator search_inc(t_content.cbegin());
    while (std::regex_search(search_inc, t_content.cend(), inc_match, include_regex)) {
        const std::string inc_file = inc_match[1].matched ? inc_match[1].str() : inc_match[2].str();
        const std::string resolved_inc = resolveAgainstBase(script_dir, inc_file);

        std::ifstream ifs(resolved_inc);
        if (ifs.is_open()) {
            std::string inc_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            preScanFile(resolved_inc, inc_content, tp_script_engine, tp_repl_module, t_db_preamble, t_scanned_files, t_loaded_schemas);
        }
        search_inc = inc_match.suffix().first;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// scanSchemaLoads — find `var.loadSclSchema(...)` / `loadSchema(...)` /
// `loadJsonSchema(...)` calls, resolve their argument, load the schema
// (file or inline content), and register it if successful.
//
// This is a best-effort pre-pass so typed DataBlock@ globals exist before
// the real compile. Schema-load failures are reported to stderr here so
// the user knows _why_ a type wasn't registered (rather than seeing a
// confusing AngelScript "undeclared identifier" later), but the pre-scan
// itself continues to the next match so one bad schema doesn't block
// unrelated ones in the same file.
// ─────────────────────────────────────────────────────────────────────────────
static void scanSchemaLoads(const std::string& t_content, const std::string& t_abs_path, asIScriptEngine* tp_script_engine,
    asIScriptModule* tp_repl_module, std::string& t_db_preamble, std::set<std::string>& t_loaded_schemas) {
    static const std::regex load_call_regex(R"(([a-zA-Z0-9_]+)\s*\.\s*(loadSclSchema|loadSchema|loadJsonSchema)\s*\(\s*([^)]+)\s*\))");

    const fs::path script_dir = std::filesystem::path(t_abs_path).parent_path();

    std::smatch match;
    std::string::const_iterator search_start(t_content.cbegin());
    while (std::regex_search(search_start, t_content.cend(), match, load_call_regex)) {
        const std::string client_var = match[1].str();
        const std::string method = match[2].str();
        const std::string raw_arg = sgrn::utils::strings::trim(match[3].str());

        if (auto literal = extractLiteralOrConstArg(t_content, raw_arg)) {
            if (literal.hasError()) {
                fmt::print(stderr, fg(fmt::color::red), "[s7shell] Literal extraction error: {}\n", literal.error());
            } else {
                auto store_res = loadScannedSchema(method, literal.value(), script_dir, t_loaded_schemas);
                if (store_res.hasError()) {
                    fmt::print(stderr, fg(fmt::color::red), "[s7shell] Schema compilation failed for {}('{}'): {}\n", method,
                        literal.value(), toString(store_res.error()));
                } else {
                    const auto& [store, schema_key] = store_res.value();
                    t_loaded_schemas.insert(schema_key);
                    registerScannedSchema(tp_script_engine, tp_repl_module, store, client_var, t_db_preamble);
                }
            }
        }

        search_start = match.suffix().first;
    }
}
// ─────────────────────────────────────────────────────────────────────────────
// scanPlcRuntimeConstructors — find `PlcRuntime(...)` constructor calls and
// load the schema from the string argument (literal or const string).
// ─────────────────────────────────────────────────────────────────────────────
static void scanPlcRuntimeConstructors(const std::string& t_content, const std::string& t_abs_path, asIScriptEngine* tp_script_engine,
    asIScriptModule* tp_repl_module, std::string& t_db_preamble, std::set<std::string>& t_loaded_schemas) {
    static const std::regex ctor_regex(R"(\bPlcRuntime\s*\(\s*([^)]+)\s*\))");

    const fs::path script_dir = std::filesystem::path(t_abs_path).parent_path();

    std::smatch match;
    std::string::const_iterator search_start(t_content.cbegin());
    while (std::regex_search(search_start, t_content.cend(), match, ctor_regex)) {
        const std::string raw_arg = sgrn::utils::strings::trim(match[1].str());

        if (auto literal = extractLiteralOrConstArg(t_content, raw_arg)) {
            // Treat the constructor argument as an SCL schema (file path or inline text).
            // loadScannedSchema will attempt to resolve it as a file relative to the script
            // and fall back to inline content if no file exists.
            if (literal.hasError()) {
                fmt::print(stderr, fg(fmt::color::red), "[s7shell] Literal extraction error: {}\n", literal.error());
            } else {
                auto store_res = loadScannedSchema("loadSclSchema", *literal, script_dir, t_loaded_schemas);
                if (store_res.hasError()) {
                    fmt::print(stderr, fg(fmt::color::red), "[s7shell] Schema compilation failed for PlcRuntime('{}'): {}\n", *literal,
                        toString(store_res.error()));
                } else {
                    const auto& [store, schema_key] = store_res.value();
                    t_loaded_schemas.insert(schema_key);
                    registerScannedSchema(tp_script_engine, tp_repl_module, store, "plc", t_db_preamble);
                }
            }
        }

        search_start = match.suffix().first;
    }
}
// ─────────────────────────────────────────────────────────────────────────────
// preScanFile — top-level entry point: dedupe by absolute path, then scan
// includes (recursively) followed by schema-load calls in this file.
// ─────────────────────────────────────────────────────────────────────────────
static void preScanFile(const std::string& t_filename, const std::string& t_content, asIScriptEngine* tp_script_engine,
    asIScriptModule* tp_repl_module, std::string& t_db_preamble, std::set<std::string>& t_scanned_files,
    std::set<std::string>& t_loaded_schemas) {
    const std::string abs_path = sgrn::utils::filesystem::expandUserPath(fs::absolute(t_filename).string());
    if (t_scanned_files.count(abs_path))
        return;
    t_scanned_files.insert(abs_path);

    scanIncludes(t_content, abs_path, tp_script_engine, tp_repl_module, t_db_preamble, t_scanned_files, t_loaded_schemas);
    scanSchemaLoads(t_content, abs_path, tp_script_engine, tp_repl_module, t_db_preamble, t_loaded_schemas);
    scanPlcRuntimeConstructors(t_content, abs_path, tp_script_engine, tp_repl_module, t_db_preamble, t_loaded_schemas);
}
void S7Shell::runScript(const std::string& t_filename) {
    std::ifstream ifs(t_filename);
    if (!ifs.is_open()) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Cannot open script: '{}'\n", t_filename);
        return;
    }
    std::string t_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    // Support for 'import "env.as";'
    t_content =
        std::regex_replace(t_content, std::regex(R"(^\s*import\s+["']([^"']+)["']\s*;?)", std::regex::multiline), "#include \"$1\"");

    std::set<std::string> t_scanned_files;
    preScanFile(t_filename, t_content, p_script_engine_, p_repl_module_, db_preamble_, t_scanned_files, loaded_schemas_);

    // Build — prepend auto-generated DB references as an anonymous section
    CScriptBuilder builder;
    if (builder.StartNewModule(p_script_engine_, "main") < 0) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Failed to create module for '{}'\n", t_filename);
        return;
    }
    // Preamble: auto-generated DataBlock@ handles + init script content
    if (!db_preamble_.empty())
        builder.AddSectionFromMemory("<db_refs>", db_preamble_.c_str());

    // Add the modified content from memory to support our rewritten imports
    if (builder.AddSectionFromMemory(t_filename.c_str(), t_content.c_str()) < 0) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Failed to add script section: '{}'\n", t_filename);
        return;
    }
    if (builder.BuildModule() < 0) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Compilation failed: '{}'\n", t_filename);
        return;
    }

    asIScriptModule* p_mod = p_script_engine_->GetModule("main");
    asIScriptFunction* p_func = p_mod->GetFunctionByDecl("void main()");
    if (!p_func) {
        fmt::print(fg(fmt::color::green), "[s7shell] Loaded '{}' into REPL (no main() executed)\n", t_filename);
        return;
    }

    asIScriptContext* p_ctx = p_script_engine_->CreateContext();
    p_ctx->Prepare(p_func);
    const int r = p_ctx->Execute();

    if (r == asEXECUTION_EXCEPTION) {
        // Structured cascade: where + what
        const char* p_fn = "?";
        int line = 0, col = 0;
        if (asIScriptFunction* p_ef = p_ctx->GetExceptionFunction()) {
            p_fn = p_ef->GetName();
            line = p_ctx->GetExceptionLineNumber(&col);
        }
        fmt::print(stderr, fg(fmt::color::red),
            "[s7shell] Runtime exception in {}() at {}:{}\n"
            "          → {}\n",
            p_fn, line, col, p_ctx->GetExceptionString());
    } else if (r != asEXECUTION_FINISHED) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Execution ended with unexpected state (code {})\n", r);
    }

    p_ctx->Release();
    p_script_engine_->DiscardModule("main");
}

// ─────────────────────────────────────────────────────────────────────────────
// runScripts() — compile multiple scripts as sections of a single module.
//
// All files are pre-scanned for loadSclSchema() calls first so that schema
// types (ReactorCore, PrimaryCoolant, …) are registered BEFORE compilation.
// Globals and functions declared in earlier files are then visible to later
// ones, enabling the env.as + nuclear_simulation.as two-file pattern.
// ─────────────────────────────────────────────────────────────────────────────
static void loadAllFiles(const std::vector<std::string>& t_filenames, std::vector<std::string> t_contents) {
    t_contents.reserve(t_filenames.size());
    for (const auto& p_fn : t_filenames) {
        std::ifstream ifs(p_fn);
        if (!ifs.is_open()) {
            fmt::print(stderr, fg(fmt::color::red), "[s7shell] Cannot open script: '{}'\n", p_fn);
            return;
        }
        std::string raw_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        // Support for 'import "env.as";'
        t_contents.emplace_back(
            std::regex_replace(raw_content, std::regex(R"(^\s*import\s+["']([^"']+)["']\s*;?)", std::regex::multiline), "#include \"$1\""));
    }
}
void S7Shell::runScripts(const std::vector<std::string>& t_filenames) {
    if (t_filenames.empty())
        return;
    if (t_filenames.size() == 1) {
        runScript(t_filenames[0]);
        return;
    }

    std::vector<std::string> contents;
    loadAllFiles(t_filenames, contents);
    // ── Step 2: pre-scan ALL files for loadSclSchema calls ───────────────────
    std::set<std::string> t_scanned_files;
    for (size_t i = 0; i < t_filenames.size(); ++i) {
        preScanFile(t_filenames[i], contents[i], p_script_engine_, p_repl_module_, db_preamble_, t_scanned_files, loaded_schemas_);
    }

    // ── Step 3: compile all sections into a single module ────────────────────
    CScriptBuilder builder;
    if (builder.StartNewModule(p_script_engine_, "main") < 0) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Failed to create merged module\n");
        return;
    }
    if (!db_preamble_.empty())
        builder.AddSectionFromMemory("<db_refs>", db_preamble_.c_str());
    for (size_t i = 0; i < t_filenames.size(); ++i) {
        // Write to a temp file so CScriptBuilder can add it by memory
        const std::string section_name = fs::path(t_filenames[i]).filename().string();
        if (builder.AddSectionFromMemory(section_name.c_str(), contents[i].c_str()) < 0) {
            fmt::print(stderr, fg(fmt::color::red), "[s7shell] Failed to add section: '{}'\n", t_filenames[i]);
            return;
        }
    }
    if (builder.BuildModule() < 0) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Compilation failed for merged scripts\n");
        return;
    }

    // ── Step 4: run main() ───────────────────────────────────────────────────
    asIScriptModule* p_mod = p_script_engine_->GetModule("main");
    asIScriptFunction* p_fn = p_mod->GetFunctionByDecl("void main()");
    if (!p_fn) {
        fmt::print(fg(fmt::color::green), "[s7shell] Loaded merged scripts into REPL (no main() executed)\n");
        return;
    }
    asIScriptContext* p_ctx = p_script_engine_->CreateContext();
    p_ctx->Prepare(p_fn);
    const int r = p_ctx->Execute();
    if (r == asEXECUTION_EXCEPTION) {
        const char* p_ef_name = "?";
        int ln = 0, col = 0;
        if (auto* p_ef = p_ctx->GetExceptionFunction()) {
            p_ef_name = p_ef->GetName();
            ln = p_ctx->GetExceptionLineNumber(&col);
        }
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Runtime exception in {}() at {}:{}\n          → {}\n", p_ef_name, ln, col,
            p_ctx->GetExceptionString());
    } else if (r != asEXECUTION_FINISHED) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Execution ended with unexpected state (code {})\n", r);
    }
    p_ctx->Release();
    p_script_engine_->DiscardModule("main");
}
} // namespace sgrn::s7shell::shell
