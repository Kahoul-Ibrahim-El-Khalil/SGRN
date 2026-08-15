// ============================================================================
// sclc — Symbolic Communication Language Compiler
//
// Standalone CLI tool for compiling PLC schematics into canonical registries.
// Part of the sgrn::scl library — can be published independently.
//
// Subcommands:
//   compile   — Parse .scl/.udt/.db/.xml/.json and emit JSON registry
//   codegen   — Generate s7codec-compatible C++ headers from compiled schema
//   emit-scl  — Generate clean .scl source files from a compiled registry
//   emit-dir  — Emit canonical directory layout (UDT{idx}-{name}.udt, DB{idx}-{name}.db)
//   examples  — Generate example .scl/.udt files
//   man       — Print SCL syntax reference manual
// ============================================================================

#include <fmt/core.h>
#include <sgrn/scl/schema/DbSymbolsParser.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/schema/SclCompiler.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <algorithm>
#include <cxxopts.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using sgrn::scl::Err;
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
    fmt::print(R"(USAGE:
    sclc <command> [options]

COMMANDS:
    compile       Parse .scl/.udt/.db/.xml/.json files and emit a JSON registry.
                  Aggregates multiple source files into one canonical schema.

    codegen       Generate s7codec-compatible C++ header from a compiled schema.
                  Emits DATABLOCK() structs, S7_BIT_GROUP packing, arrays,
                  S7RawString<N>, S7RawDTL, and UDT composition.

    emit-scl      Generate clean .scl source files from a compiled registry.
                  Round-trip safe — output can be fed back into sclc.

    emit-dir      Emit canonical directory layout with normalized filenames:
                    UDT{{idx}}-{{name}}.udt, DB{{idx}}-{{name}}.db, registry.json

    examples      Generate example .scl and .udt files to get started.

    man           Print the SCL syntax reference manual.

EXAMPLES:
    sclc compile --parse ./symbols/ -o registry.json
    sclc codegen --parse ./symbols/ -o plc_schema.hpp
    sclc compile --file Motor.udt --file Motors.scl -o registry.json
    sclc compile --parse ./symbols/ --debug
    sclc emit-scl -i registry.json -o ./output/
    sclc emit-dir --parse ./symbols/ -o ./canonical/
    sclc examples -o ./examples/

CANONICAL NAMING:
    When emitting canonical directories, files follow the convention:
      UDT1-MotorData.udt    (TYPE "MotorData", UDT number 1)
      DB10-Motors.db         (DATA_BLOCK "Motors", DB number 10)
      registry.json          (compiled canonical JSON schema)

    Parsing canonical filenames:
      UDT1-MotorData.udt  →  {{index: 1, name: "MotorData"}}
      DB10-Motors.db       →  {{index: 10, name: "Motors"}}

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
      #UNIT "rpm"            Engineering unit annotation
      #EVENT_TRIGGER         Enable OPC UA event emission for this node

   C. Retentivity
      RETAIN / NON_RETAIN    Controls persistence behavior

4. CANONICAL DIRECTORY LAYOUT
   When using 'emit-dir', sclc normalizes files to:
     UDT{number}-{name}.udt   (e.g., UDT1-MotorData.udt)
     DB{number}-{name}.db     (e.g., DB10-Motors.db)
     registry.json             (compiled JSON schema)

   This convention allows round-tripping:
     sclc compile --parse ./canonical/ -o registry.json
     sclc emit-dir -i registry.json -o ./canonical-v2/

================================================================================
)");
}

// ── Compile Command ─────────────────────────────────────────────────────────

int cmdCompile(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc compile", "Parse symbol files and emit a JSON registry.");
    opts.add_options()("p,parse", "Directory or file to parse", cxxopts::value<std::string>())("s,schema",
        "Alias for --parse (Directory or file to parse)",
        cxxopts::value<std::string>())("f,file", "One or more symbol files", cxxopts::value<std::vector<std::string>>())(
        "o,output", "Output JSON registry file (stdout if omitted)", cxxopts::value<std::string>()->default_value(""))(
        "force", "Overwrite duplicate DB/UDT entries", cxxopts::value<bool>()->default_value("false"))(
        "debug", "Print parsed structure to stdout", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    auto res = opts.parse(t_argc, tp_argv);

    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    const bool force = res["force"].as<bool>();
    const bool debug = res["debug"].as<bool>();

    PlcSchemaStore registry;

    if (res.count("parse") || res.count("schema")) {
        std::string input_path =
            sgrn::utils::filesystem::expandUserPath(res.count("schema") ? res["schema"].as<std::string>() : res["parse"].as<std::string>());

        if (fs::is_directory(input_path)) {
            auto store_res = SclCompiler::compileDirectory(input_path, {.force = force});
            if (store_res.hasError()) {
                fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
                return EXIT_FAILURE;
            }
            registry = std::move(store_res.value());
        } else {
            auto store_res = SclCompiler::compileFile(input_path, {.force = force});
            if (store_res.hasError()) {
                fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
                return EXIT_FAILURE;
            }
            registry = std::move(store_res.value());
        }
    } else if (res.count("file")) {
        auto files = res["file"].as<std::vector<std::string>>();
        for (auto& f : files)
            f = sgrn::utils::filesystem::expandUserPath(f);
        auto store_res = SclCompiler::compileFiles(files, {.force = force});
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else {
        fmt::print(stderr, "\033[31merror:\033[0m provide --parse <dir|file> or --file <path>\n");
        fmt::print("{}\n", opts.help());
        return EXIT_FAILURE;
    }

    printWarnings(registry);

    // Summary
    fmt::print(stderr, "\033[32mcompiled:\033[0m {} DBs, {} UDTs, {} tags\n", registry.availableDbs().size(), registry.udts().size(),
        registry.tags().size());

    std::string json = registry.toJson(std::nullopt, false, true);

    if (debug || res["output"].as<std::string>().empty()) {
        fmt::print("{}\n", json);
        return EXIT_SUCCESS;
    }

    const std::string output_str = res["output"].as<std::string>();
    if (!sgrn::utils::filesystem::writeStringToFile(output_str, json)) {
        fmt::print(stderr, "\033[31merror:\033[0m failed to write {}\n", output_str);
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32mwrote:\033[0m {}\n", output_str);
    return EXIT_SUCCESS;
}

// ── Emit SCL Command ────────────────────────────────────────────────────────

int cmdEmitScl(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc emit-scl", "Generate clean .scl source files from a schema.");
    opts.add_options()("p,parse", "Directory or file to compile first", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "o,output", "Output directory for .scl files", cxxopts::value<std::string>()->default_value("./scl-output"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    PlcSchemaStore registry;
    const bool force = res["force"].as<bool>();

    if (res.count("input")) {
        auto store_res = PlcSchemaStore::loadFromJsonFile(res["input"].as<std::string>());
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else if (res.count("parse") || res.count("schema")) {
        std::string input =
            sgrn::utils::filesystem::expandUserPath(res.count("schema") ? res["schema"].as<std::string>() : res["parse"].as<std::string>());
        auto store_res = fs::is_directory(input) ? SclCompiler::compileDirectory(input, {.force = force})
                                                 : SclCompiler::compileFile(input, {.force = force});
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else {
        fmt::print(stderr, "\033[31merror:\033[0m provide --input <json> or --parse <dir|file>\n");
        return EXIT_FAILURE;
    }

    printWarnings(registry);

    auto emit_res = SclCompiler::emitScl(registry, res["output"].as<std::string>());
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error().string());
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32memitted:\033[0m .scl files to {}\n", res["output"].as<std::string>());
    return EXIT_SUCCESS;
}

// ── Emit Canonical Directory Command ────────────────────────────────────────

int cmdEmitDir(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc emit-dir", "Emit canonical directory layout.");
    opts.add_options()("p,parse", "Directory or file to compile first", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "o,output", "Output directory", cxxopts::value<std::string>()->default_value("./canonical"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    PlcSchemaStore registry;
    const bool force = res["force"].as<bool>();

    if (res.count("input")) {
        auto store_res = PlcSchemaStore::loadFromJsonFile(res["input"].as<std::string>());
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else if (res.count("parse") || res.count("schema")) {
        std::string input =
            sgrn::utils::filesystem::expandUserPath(res.count("schema") ? res["schema"].as<std::string>() : res["parse"].as<std::string>());
        auto store_res = fs::is_directory(input) ? SclCompiler::compileDirectory(input, {.force = force})
                                                 : SclCompiler::compileFile(input, {.force = force});
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else {
        fmt::print(stderr, "\033[31merror:\033[0m provide --input <json> or --parse <dir|file>\n");
        return EXIT_FAILURE;
    }

    printWarnings(registry);

    auto emit_res = SclCompiler::emitCanonical(registry, res["output"].as<std::string>());
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error().string());
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32memitted:\033[0m canonical directory to {}\n", res["output"].as<std::string>());
    return EXIT_SUCCESS;
}

// ── Examples Command ────────────────────────────────────────────────────────

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

// ── Codegen Command ─────────────────────────────────────────────────────────

int cmdCodegen(int t_argc, char** tp_argv) {
    cxxopts::Options opts("sclc codegen", "Generate s7codec-compatible C++ header.");
    opts.add_options()("p,parse", "Directory or file to compile", cxxopts::value<std::string>())("s,schema", "Alias for --parse",
        cxxopts::value<std::string>())("i,input", "Input JSON registry file", cxxopts::value<std::string>())(
        "o,output", "Output .hpp file (stdout if omitted)", cxxopts::value<std::string>()->default_value(""))(
        "guard", "Header guard prefix", cxxopts::value<std::string>()->default_value("SCLC_GENERATED"))(
        "force", "Overwrite existing entries", cxxopts::value<bool>()->default_value("false"))("h,help", "Print help");

    auto res = opts.parse(t_argc, tp_argv);
    if (res.count("help")) {
        fmt::print("{}\n", opts.help());
        return EXIT_SUCCESS;
    }

    PlcSchemaStore registry;
    const bool force = res["force"].as<bool>();

    if (res.count("input")) {
        auto store_res = PlcSchemaStore::loadFromJsonFile(res["input"].as<std::string>());
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else if (res.count("parse") || res.count("schema")) {
        std::string input =
            sgrn::utils::filesystem::expandUserPath(res.count("schema") ? res["schema"].as<std::string>() : res["parse"].as<std::string>());
        auto store_res = fs::is_directory(input) ? SclCompiler::compileDirectory(input, {.force = force})
                                                 : SclCompiler::compileFile(input, {.force = force});
        if (store_res.hasError()) {
            fmt::print(stderr, "\033[31merror:\033[0m {}\n", store_res.error().string());
            return EXIT_FAILURE;
        }
        registry = std::move(store_res.value());
    } else {
        fmt::print(stderr, "\033[31merror:\033[0m provide --input <json> or --parse <dir|file>\n");
        return EXIT_FAILURE;
    }

    printWarnings(registry);

    std::string guard_prefix = res["guard"].as<std::string>();
    std::string output_path = res["output"].as<std::string>();

    if (output_path.empty()) {
        // Print to stdout
        fmt::print("{}", SclCompiler::emitCppHeader(registry, guard_prefix));
        return EXIT_SUCCESS;
    }

    auto emit_res = SclCompiler::emitCpp(registry, output_path, guard_prefix);
    if (emit_res.hasError()) {
        fmt::print(stderr, "\033[31merror:\033[0m {}\n", emit_res.error().string());
        return EXIT_FAILURE;
    }
    fmt::print(stderr, "\033[32mgenerated:\033[0m {}\n", output_path);
    return EXIT_SUCCESS;
}

} // namespace

// ── Main dispatcher ─────────────────────────────────────────────────────────

int main_cb(int t_argc, char** tp_argv) {
    if (t_argc < 2) {
        printUsage();
        return EXIT_FAILURE;
    }

    std::string command = tp_argv[1];

    // Shift argv for subcommand parsing
    int sub_argc = t_argc - 1;
    char** p_sub_argv = tp_argv + 1;

    if (command == "compile")
        return cmdCompile(sub_argc, p_sub_argv);
    if (command == "codegen")
        return cmdCodegen(sub_argc, p_sub_argv);
    if (command == "emit-scl")
        return cmdEmitScl(sub_argc, p_sub_argv);
    if (command == "emit-dir")
        return cmdEmitDir(sub_argc, p_sub_argv);
    if (command == "examples")
        return cmdExamples(sub_argc, p_sub_argv);
    if (command == "man") {
        printManPage();
        return EXIT_SUCCESS;
    }
    if (command == "-h" || command == "--help") {
        printUsage();
        return EXIT_SUCCESS;
    }

    fmt::print(stderr, "\033[31merror:\033[0m unknown command '{}'\n\n", command);
    printUsage();
    return EXIT_FAILURE;
}

int main(int t_argc, char** tp_argv) {
    return sgrn::utils::app::runMain(t_argc, tp_argv, main_cb, "sclc");
}
