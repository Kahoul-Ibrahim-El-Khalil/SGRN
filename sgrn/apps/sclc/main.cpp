// ============================================================================
// sclc — Symbolic Communication Language Compiler
//
// Standalone CLI tool for compiling PLC schematics into canonical registries.
// Part of the sgrn::scl library — can be published independently.
//
// Subcommands & Aliases:
//   compile (cmp, c)            — Parse inputs and emit JSON registry [DEFAULT]
//   codegen (gen, header, cpp)  — Generate s7codec-compatible C++ headers
//   emit-scl (scl)              — Generate clean .scl source files
//   emit-dir (dir, canonical)   — Emit normalized directory layout
//   emit-angelscript (emit-as, as) — Generate AngelScript header declarations
//   examples                    — Generate example .scl/.udt files
//   man                         — Print SCL syntax reference manual
// ============================================================================

#include <fmt/core.h>
#include <sgrn/scl/AsApiEmitter.hpp>
#include <sgrn/scl/schema/DbSymbolsParser.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/schema/SclCompiler.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <algorithm>
#include <cxxopts.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using sgrn::scl::PlcSchemaStore;
using sgrn::scl::SclCompiler;

// ── Helpers ─────────────────────────────────────────────────────────────────

namespace
{

void printWarnings(const PlcSchemaStore& t_store) {
    for (const auto& warning : t_store.warnings())
        fmt::print(stderr, "\033[33mwarning:\033[0m {}\n", warning);
}

void printBanner() {
    fmt::print(R"(
  ┌─────────────────────────────────────────────────────────────────┐
  │  sclc — Siemens Control Language Datablock declarative Compiler │
  │  Part of the SGRN Industrial Gateway Suite                      │
  └─────────────────────────────────────────────────────────────────┘

)");
}

void printUsage() {
    printBanner();
    fmt::print("{}", R"(USAGE:
    sclc [command] [inputs...] [options]

COMMANDS & ALIASES:
    compile (cmp, c)            Parse SCL/UDT/DB/XML/JSON inputs and emit JSON registry.
                                [DEFAULT subcommand if omitted]

    codegen (gen, header, cpp)  Generate s7codec-compatible C++ header from schema.

    emit-scl (scl)              Generate clean .scl source files from schema.

    emit-dir (dir, canonical)   Emit normalized directory layout:
                                  UDT{idx}-{name}.udt, DB{idx}-{name}.db, registry.json

    emit-angelscript (emit-as, as)
                                Generate declaration-only AngelScript .as headers.

    examples                    Generate starter .scl/.udt files.

    man                         Print SCL syntax reference manual.

OPTIONS:
    -o, --output <path>         Output file or directory ('-' for stdout)
    -f, --force                 Overwrite existing DB/UDT entries or files
    -v, --verbose / --debug     Enable debug/verbose output
    -h, --help                  Print help

EXAMPLES:
    sclc ./symbols/ -o registry.json
    sclc Motor.udt Motors.scl -o registry.json
    sclc Motor.scl                              # prints compiled JSON to stdout
    cat Motor.scl | sclc -                      # read stdin, print JSON to stdout
    sclc gen ./symbols/ -o plc_schema.hpp
    sclc as ./symbols/ -o ./generated/
    sclc as plant.scl -o ./generated/ --include-shell-api
    sclc as plant.scl -o ./generated/ --include-predefined
    sclc scl registry.json -o ./scl-output/
    sclc dir ./symbols/ -o ./canonical/

For detailed SCL syntax reference, run:  sclc man
)");
}

void printManPage() {
    fmt::print("{}", R"(================================================================================
SIEMENS S7 SCL SYNTAX & INFORMATION MODEL MAPPING MANUAL
================================================================================

1. OVERVIEW
   sclc (SCL Compiler) parses Structured Control Language (SCL) declarations
   from Siemens S7 Data Blocks (.scl) and User-Defined Types (.udt), and
   compiles them into a canonical JSON registry for use by industrial gateways.

   Supported input formats:
     .scl  — SCL source (DB/UDT declarations)
     .udt  — UDT type definitions
     .db   — Data block exports
     .xml  — TIA Portal tag table exports
     .json — Pre-compiled JSON registries (for merging)
     -     — Standard input (stdin)

2. SIEMENS SCL TYPE SYSTEM & DECLARATIONS

   A. Elementary Primitive Types
      - Bool            : 1-bit boolean flag
      - Byte / USInt    : 8-bit unsigned
      - SInt            : 8-bit signed
      - Word / UInt     : 16-bit unsigned
      - Int             : 16-bit signed
      - DWord / UDInt   : 32-bit unsigned
      - DInt            : 32-bit signed
      - LWord / ULInt   : 64-bit unsigned
      - LInt            : 64-bit signed
      - Real            : 32-bit IEEE 754 float
      - LReal           : 64-bit IEEE 754 double
      - Char            : 8-bit ASCII character
      - String[len]     : Variable length string (max 254 + 2B header)
      - WString[len]    : Wide string (max 16382 + 4B header)
      - Time            : 32-bit signed milliseconds
      - Date            : 16-bit days since 1990-01-01
      - DTL             : 12-byte structured timestamp

   B. Structured Complex Types
      - STRUCT / END_STRUCT : Inline structure definition
      - TYPE / END_TYPE     : User-Defined Type (reusable template)

   C. Array Declarations
      ARRAY [ <lower> .. <upper> ] OF <type>
      Examples:
        Motors : ARRAY[0..3] OF "UdtMotor";
        Pressures : ARRAY[0..7] OF Real;

3. EXTENDED DIRECTIVES & PRAGMAS

   A. Attribute Directives ({ ... })
      { S7_Optimized_Access := 'FALSE' }
      { S7_SetPoint := 'True' }

    B. Pragma Metadata (#)
       #BIG_ENDIAN            Big-endian encoding
       #LITTLE_ENDIAN         Little-endian encoding
       #UNIT "rpm"            Engineering unit annotation (HOW measured)
       #DIMENSION "pressure"  Dimension class (WHAT measured)
       #DIMENSIONS("a", …)    File-top dimension vocabulary (undeclared fails)
       #DESC "…"              Human description (dashboard tooltip)
       #LABEL                 Label/metadata marker (flagged, never a feature)
       #PRECISION(n)          Dashboard display decimals
       #NOMINAL(v)            Expected operating point (residual reference)
       #TRANSIENT             Excluded from JSONL WAL + datasets
       #READ_ONLY             Semantic POST writes denied
       #ALARM(lo, hi)         Acceptable band (surfaced, not evaluated)
       #EVENT_TRIGGER         Enable OPC UA event emission for this node

   C. Retentivity
      RETAIN / NON_RETAIN    Controls persistence behavior

4. CANONICAL DIRECTORY LAYOUT
   When using 'emit-dir', sclc normalizes files to:
     UDT{number}-{name}.udt   (e.g., UDT1-MotorData.udt)
     DB{number}-{name}.db     (e.g., DB10-Motors.db)
     registry.json             (compiled JSON schema)

================================================================================
)");
}

// ── Path Resolution Helper ──────────────────────────────────────────────────

std::vector<std::string> resolveInputPaths(const cxxopts::ParseResult& res) {
    std::vector<std::string> paths;

    if (res.count("inputs")) {
        auto vec = res["inputs"].as<std::vector<std::string>>();
        for (const auto& p : vec)
            paths.push_back(p == "-" ? "-" : sgrn::utils::filesystem::expandUserPath(p));
    }

    bool used_deprecated = false;

    if (res.count("parse") || res.count("schema")) {
        used_deprecated = true;
        std::string p = res.count("schema") ? res["schema"].as<std::string>() : res["parse"].as<std::string>();
        paths.push_back(p == "-" ? "-" : sgrn::utils::filesystem::expandUserPath(p));
    }
    if (res.count("file")) {
        used_deprecated = true;
        auto vec = res["file"].as<std::vector<std::string>>();
        for (const auto& p : vec)
            paths.push_back(p == "-" ? "-" : sgrn::utils::filesystem::expandUserPath(p));
    }
    if (res.count("input")) {
        used_deprecated = true;
        std::string p = res["input"].as<std::string>();
        paths.push_back(p == "-" ? "-" : sgrn::utils::filesystem::expandUserPath(p));
    }

    if (used_deprecated) {
        fmt::print(stderr, "\033[33mwarning:\033[0m option '--parse'/'--schema'/'--file'/'--input' is deprecated. "
                           "Pass positional input arguments instead (e.g. 'sclc <inputs...>').\n");
    }

    return paths;
}

sgrn::Result<PlcSchemaStore, ::sgrn::scl::SclError> compileInputs(const std::vector<std::string>& paths, bool force) {

    if (paths.empty()) {
        return ::sgrn::scl::SclError::Generic;
    }

    if (paths.size() == 1) {
        const std::string& path = paths[0];
        if (path != "-" && fs::is_directory(path)) {
            return SclCompiler::compileDirectory(path, {.force = force});
        } else {
            return SclCompiler::compileFile(path, {.force = force});
        }
    }

    return SclCompiler::compileFiles(paths, {.force = force});
}

// ── Subcommand Handlers ─────────────────────────────────────────────────────

int cmdCompile(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc compile", "Parse symbol files and emit a JSON registry.");
    opts.add_options()("inputs", "Input files or directories ('-' for stdin)", cxxopts::value<std::vector<std::string>>())("p,parse",
        "Directory or file to parse",
        cxxopts::value<std::string>())("s,schema", "Alias for --parse", cxxopts::value<std::string>())("f,file", "One or more symbol files",
        cxxopts::value<std::vector<std::string>>())("i,input", "Input JSON registry file or symbol path", cxxopts::value<std::string>())(
        "o,output", "Output JSON file ('-' for stdout)", cxxopts::value<std::string>()->default_value(""))(
        "force", "Overwrite duplicate DB/UDT entries", cxxopts::value<bool>()->default_value("false"))(
        "debug", "Print parsed structure to stdout", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    opts.parse_positional("inputs");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    auto inputs = resolveInputPaths(res);
    if (inputs.empty()) {
        fmt::print(stderr, "\033[31merror:\033[0m no input files or directories specified\n");
        fmt::print("{}\n", opts.help());
        return EXIT_FAILURE;
    }

    const bool force = res["force"].as<bool>();
    const bool debug = res["debug"].as<bool>();

    auto store_res = compileInputs(inputs, force);
    if (store_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error());
        return EXIT_FAILURE;
    }

    PlcSchemaStore registry = std::move(store_res.value());
    printWarnings(registry);

    // Summary output to stderr
    fmt::print(stderr, "\033[32mcompiled:\033[0m {} DBs, {} UDTs, {} tags\n", registry.availableDbs().size(), registry.udts().size(),
        registry.tags().size());

    std::string json = registry.toJson(std::nullopt, false, true);

    std::string output_str = res["output"].as<std::string>();
    if (debug || output_str.empty() || output_str == "-") {
        fmt::print("{}\n", json);
        return EXIT_SUCCESS;
    }

    if (!sgrn::utils::filesystem::writeStringToFile(output_str, json)) {
        fmt::print(stderr, "\033[31merror:\033[0m failed to write {}\n", output_str);
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32mwrote:\033[0m {}\n", output_str);
    return EXIT_SUCCESS;
}

int cmdCodegen(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc codegen", "Generate s7codec-compatible C++ header.");
    opts.add_options()("inputs", "Input files, directories, or JSON registry ('-' for stdin)", cxxopts::value<std::vector<std::string>>())(
        "p,parse", "Directory or file to compile", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "f,file", "One or more symbol files", cxxopts::value<std::vector<std::string>>())(
        "o,output", "Output .hpp file ('-' for stdout)", cxxopts::value<std::string>()->default_value(""))(
        "guard", "Header guard prefix", cxxopts::value<std::string>()->default_value("SCLC_GENERATED"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    opts.parse_positional("inputs");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    auto inputs = resolveInputPaths(res);
    if (inputs.empty()) {
        fmt::print(stderr, "\033[31merror:\033[0m no input files or directories specified\n");
        return EXIT_FAILURE;
    }

    const bool force = res["force"].as<bool>();
    auto store_res = compileInputs(inputs, force);
    if (store_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error());
        return EXIT_FAILURE;
    }

    PlcSchemaStore registry = std::move(store_res.value());
    printWarnings(registry);

    std::string guard_prefix = res["guard"].as<std::string>();
    std::string output_path = res["output"].as<std::string>();

    if (output_path.empty() || output_path == "-") {
        fmt::print("{}", SclCompiler::emitCppHeader(registry, guard_prefix));
        return EXIT_SUCCESS;
    }

    auto emit_res = SclCompiler::emitCpp(registry, output_path, guard_prefix);
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error());
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32mgenerated:\033[0m {}\n", output_path);
    return EXIT_SUCCESS;
}

int cmdEmitScl(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc emit-scl", "Generate clean .scl source files from a schema.");
    opts.add_options()("inputs", "Input files or directories ('-' for stdin)", cxxopts::value<std::vector<std::string>>())(
        "p,parse", "Directory or file to compile first", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "f,file", "One or more symbol files", cxxopts::value<std::vector<std::string>>())(
        "o,output", "Output directory for .scl files", cxxopts::value<std::string>()->default_value("./scl-output"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    opts.parse_positional("inputs");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    auto inputs = resolveInputPaths(res);
    if (inputs.empty()) {
        fmt::print(stderr, "\033[31merror:\033[0m no input files or directories specified\n");
        return EXIT_FAILURE;
    }

    const bool force = res["force"].as<bool>();
    auto store_res = compileInputs(inputs, force);
    if (store_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error());
        return EXIT_FAILURE;
    }

    PlcSchemaStore registry = std::move(store_res.value());
    printWarnings(registry);

    auto emit_res = SclCompiler::emitScl(registry, res["output"].as<std::string>());
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error());
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32memitted:\033[0m .scl files to {}\n", res["output"].as<std::string>());
    return EXIT_SUCCESS;
}

int cmdEmitDir(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc emit-dir", "Emit canonical directory layout.");
    opts.add_options()("inputs", "Input files or directories ('-' for stdin)", cxxopts::value<std::vector<std::string>>())(
        "p,parse", "Directory or file to compile first", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "f,file", "One or more symbol files", cxxopts::value<std::vector<std::string>>())(
        "o,output", "Output directory", cxxopts::value<std::string>()->default_value("./canonical"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    opts.parse_positional("inputs");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    auto inputs = resolveInputPaths(res);
    if (inputs.empty()) {
        fmt::print(stderr, "\033[31merror:\033[0m no input files or directories specified\n");
        return EXIT_FAILURE;
    }

    const bool force = res["force"].as<bool>();
    auto store_res = compileInputs(inputs, force);
    if (store_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error());
        return EXIT_FAILURE;
    }

    PlcSchemaStore registry = std::move(store_res.value());
    printWarnings(registry);

    auto emit_res = SclCompiler::emitCanonical(registry, res["output"].as<std::string>());
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error());
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32memitted:\033[0m canonical directory to {}\n", res["output"].as<std::string>());
    return EXIT_SUCCESS;
}

int cmdEmitAngelScript(int t_argc, char** tp_argv) {
    // Tolerate single-dash spellings of the multi-word long flags here
    // (e.g. `-include-predefined`); cxxopts would otherwise read them as a
    // short-option group (`-i` + attached value). Build a normalized argv
    // rather than rewriting the argument buffers in place.
    std::vector<std::string> args_norm;
    args_norm.reserve(t_argc);
    for (int i = 0; i < t_argc; ++i) {
        std::string a = tp_argv[i];
        if (a == "-include-predefined" || a == "-include-shell-api")
            a = "--" + a.substr(1);
        args_norm.push_back(std::move(a));
    }
    std::vector<const char*> argv_norm;
    argv_norm.reserve(args_norm.size());
    for (const std::string& a : args_norm)
        argv_norm.push_back(a.c_str());

    cxxopts::Options opts("sclc emit-angelscript", "Generate declaration-only AngelScript .as files.");
    opts.add_options()("inputs", "Input files or directories ('-' for stdin)", cxxopts::value<std::vector<std::string>>())(
        "p,parse", "Directory or file to compile first", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "f,file", "One or more symbol files", cxxopts::value<std::vector<std::string>>())(
        "o,output", "Output directory for .as files", cxxopts::value<std::string>()->default_value("./generated"))("include-shell-api",
        "Include the s7shell built-in API surface (writes s7shell_api.as)",
        cxxopts::value<bool>()->default_value("false"))("include-predefined",
        "Emit 'as.predefined': native API + schema surface for IDE language servers (mutually exclusive with --include-shell-api)",
        cxxopts::value<bool>()->default_value("false"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    opts.parse_positional("inputs");

    auto res = opts.parse(static_cast<int>(argv_norm.size()), argv_norm.data());
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    const bool include_shell_api = res["include-shell-api"].as<bool>();
    const bool include_predefined = res["include-predefined"].as<bool>();
    if (include_shell_api && include_predefined) {
        fmt::print(stderr, "\033[31merror:\033[0m --include-predefined and --include-shell-api are mutually exclusive: "
                           "both declare the native API surface; pass only one.\n");
        return EXIT_FAILURE;
    }

    auto inputs = resolveInputPaths(res);
    if (inputs.empty()) {
        fmt::print(stderr, "\033[31merror:\033[0m no input files or directories specified\n");
        return EXIT_FAILURE;
    }

    const bool force = res["force"].as<bool>();
    auto store_res = compileInputs(inputs, force);
    if (store_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error());
        return EXIT_FAILURE;
    }

    PlcSchemaStore registry = std::move(store_res.value());
    printWarnings(registry);

    sgrn::scl::AsEmitterOptions opts_as;
    opts_as.output_dir = res["output"].as<std::string>();
    opts_as.include_shell_api = include_shell_api;
    opts_as.include_predefined = include_predefined;

    auto emit_res = sgrn::scl::AsApiEmitter::emit(registry, opts_as);
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error());
        return EXIT_FAILURE;
    }
    const std::string written = include_predefined ? "as.predefined" : (include_shell_api ? "schema.as + s7shell_api.as" : "schema.as");
    fmt::print(stderr, "\033[32memitted:\033[0m AngelScript declarations ({}) to {}\n", written, opts_as.output_dir);
    return EXIT_SUCCESS;
}

int cmdExamples(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc examples", "Generate example .scl and .udt files.");
    opts.add_options()("o,output", "Output directory", cxxopts::value<std::string>()->default_value("./examples"))("h,help", "Print help");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    std::string output_dir = res["output"].as<std::string>();
    fs::create_directories(output_dir);

    const char* p_udt_scl = R"(TYPE "UdtMotor"
VERSION : 0.1
   STRUCT
      Running : Bool;
      Fault : Bool;
      Speed : Real;
      Current : Real;
   END_STRUCT;
END_TYPE
)";

    const char* p_db_scl = R"(DATA_BLOCK "DbTelemetry"
TITLE = Telemetry Data Block
#EVENT_TRIGGER
VERSION : 0.1
   STRUCT
      Motor1 : "UdtMotor";
      Motor2 : "UdtMotor";
      SystemOk : Bool;
      Heartbeat : DInt;
   END_STRUCT;
BEGIN
END_DATA_BLOCK
)";

    auto udt_path = fs::path(output_dir) / "UDT1-UdtMotor.udt";
    auto db_path = fs::path(output_dir) / "DB1-DbTelemetry.scl";

    if (sgrn::utils::filesystem::writeStringToFile(udt_path, p_udt_scl) && sgrn::utils::filesystem::writeStringToFile(db_path, p_db_scl)) {
        fmt::print("\033[32mgenerated:\033[0m\n  {}\n  {}\n", udt_path.string(), db_path.string());
        return EXIT_SUCCESS;
    }
    fmt::print(stderr, "\033[31merror:\033[0m failed to write to {}\n", output_dir);
    return EXIT_FAILURE;
}

} // namespace

// ── Main dispatcher ─────────────────────────────────────────────────────────

int main_cb(int t_argc, char** tp_argv) {
    if (t_argc < 2) {
        printUsage();
        return EXIT_FAILURE;
    }

    std::string command = tp_argv[1];

    if (command == "-h" || command == "--help") {
        printUsage();
        return EXIT_SUCCESS;
    }
    if (command == "man") {
        printManPage();
        return EXIT_SUCCESS;
    }

    // Explicit subcommand matching with aliases
    if (command == "compile" || command == "cmp" || command == "c")
        return cmdCompile(t_argc - 1, tp_argv + 1);
    if (command == "codegen" || command == "gen" || command == "header" || command == "cpp")
        return cmdCodegen(t_argc - 1, tp_argv + 1);
    if (command == "emit-scl" || command == "scl")
        return cmdEmitScl(t_argc - 1, tp_argv + 1);
    if (command == "emit-dir" || command == "dir" || command == "canonical")
        return cmdEmitDir(t_argc - 1, tp_argv + 1);
    if (command == "emit-angelscript" || command == "emit-as" || command == "as")
        return cmdEmitAngelScript(t_argc - 1, tp_argv + 1);
    if (command == "examples")
        return cmdExamples(t_argc - 1, tp_argv + 1);

    // If first argument is an input path, default subcommand to 'compile'
    if (!command.empty() && command[0] != '-') {
        return cmdCompile(t_argc, tp_argv);
    }

    fmt::print(stderr, "\033[31merror:\033[0m unknown command or option '{}'\n\n", command);
    printUsage();
    return EXIT_FAILURE;
}

int main(int t_argc, char** tp_argv) {
    return sgrn::utils::app::runMain(t_argc, tp_argv, main_cb, "sclc");
}
