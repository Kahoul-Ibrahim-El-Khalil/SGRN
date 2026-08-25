#pragma once

#include <sgrn/scl/types.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sgrn/scl/schema/PlcSchemaStore.hpp>

namespace sgrn::scl
{

/**
 * @brief Compilation options for the SCL compiler pipeline.
 */
struct SclCompilerOptions {
    bool force = false;               ///< Overwrite existing entries
    bool normalize_filenames = false; ///< Rename to canonical format on emit
    bool resolve_udts = true;         ///< Resolve UDT references after parsing
};

/**
 * @brief SCL Compiler — aggregates heterogeneous PLC schematics into a canonical registry.
 *
 * Supports:
 *   - Parsing .scl, .udt, .db, .xml (TIA Portal), and .json symbol files
 *   - Emitting canonical JSON registries
 *   - Emitting clean round-trip-safe .scl source files
 *   - Normalizing TIA Portal exports to canonical directory layout:
 *     UDT{index}-{name}.udt, DB{index}-{name}.db
 */
class SclCompiler {
public:
    using Options = SclCompilerOptions;

    // ── High-level compile API ───────────────────────────────────────────────

    /// Compile a single .scl/.udt/.db/.xml/.json file into a store
    static sgrn::Result<PlcSchemaStore, SchemaError> compileFile(const std::string& t_path, Options t_opts = {});

    /// Compile a directory of symbol files into a store
    static sgrn::Result<PlcSchemaStore, SchemaError> compileDirectory(const std::string& t_dir, Options t_opts = {});

    /// Compile multiple input files into a store
    static sgrn::Result<PlcSchemaStore, SchemaError> compileFiles(const std::vector<std::string>& t_paths, Options t_opts = {});

    // ── Emit outputs ─────────────────────────────────────────────────────────

    /// Emit canonical JSON registry to a file
    static sgrn::Result<void, SchemaError> emitJson(const PlcSchemaStore& t_store, const std::string& t_output_path, bool t_pretty = true);

    /// Emit clean .scl source files to a directory
    static sgrn::Result<void, SchemaError> emitScl(const PlcSchemaStore& t_store, const std::string& t_output_dir);

    /// Emit canonical directory layout (UDT{idx}-{name}.udt, DB{idx}-{name}.db)
    static sgrn::Result<void, SchemaError> emitCanonical(const PlcSchemaStore& t_store, const std::string& t_output_dir);

    // ── Canonical naming ─────────────────────────────────────────────────────

    /// "UDT1-MotorData.udt"
    static std::string canonicalUdtFilename(uint16_t t_index, const std::string& t_name);

    /// "DB10-Motors.db"
    static std::string canonicalDbFilename(uint16_t t_index, const std::string& t_name);

    /// Parse canonical filename back to (index, name). Returns nullopt on failure.
    static std::optional<std::pair<uint16_t, std::string>> parseCanonicalFilename(const std::string& t_filename);

    // ── SCL text generation ──────────────────────────────────────────────────

    /// Generate SCL text for a single UDT definition
    static std::string udtToScl(const UdtDefinition& t_udt);

    /// Generate SCL text for a single DB schema
    static std::string dbToScl(const DbSchema& t_db);

    // ── C++ code generation (s7codec compatible) ─────────────────────────────

    /// Generate a complete s7codec-compatible C++ header for the entire store.
    /// Emits DATABLOCK() structs with s7codec types, S7_BIT_GROUP packing,
    /// S7RawString<N>, S7RawDTL, arrays, and composition.
    static std::string emitCppHeader(const PlcSchemaStore& t_store, const std::string& t_guard_prefix = "SCLC_GENERATED");

    /// Emit s7codec C++ header to a file
    static sgrn::Result<void, SchemaError> emitCpp(
        const PlcSchemaStore& t_store, const std::string& t_output_path, const std::string& t_guard_prefix = "SCLC_GENERATED");

    // ── Low-level parse API (preserved from S7RegistryParser) ────────────────

    /// Parses a single UDT source file.
    static sgrn::Result<UdtDefinition, SchemaError> parseUdtFile(
        const std::string& t_path, std::map<std::string, UdtDefinition>* tp_global_udts = nullptr);

    /// Parses a single DB source file.
    static sgrn::Result<DbSchema, SchemaError> parseDbFile(
        const std::string& t_path, std::map<std::string, UdtDefinition>* tp_global_udts = nullptr);

    /// Parses a TIA Portal Tag Table XML file.
    static sgrn::Result<std::vector<PlcTag>, SchemaError> parseTagTableXmlFile(const std::string& t_path);

    /// Populates a registry from a directory of symbols.
    static sgrn::Result<void, SchemaError> loadFromDirectory(
        PlcSchemaStore& t_registry, const std::string& t_symbols_dir, bool t_force = false);

    /// Populates a registry from a list of files.
    static sgrn::Result<void, SchemaError> loadFromFiles(
        PlcSchemaStore& t_registry, const std::vector<std::string>& t_filepaths, bool t_force = false);

    /// Loads a single file (JSON, XML, or text symbol) into the registry.
    static sgrn::Result<void, SchemaError> loadFile(PlcSchemaStore& t_registry, const std::string& t_filepath, bool t_force = false,
        std::map<std::string, UdtDefinition>* tp_global_udts = nullptr);

    /// Loads raw schema content (JSON or text symbol) into the registry.
    static sgrn::Result<void, SchemaError> loadFromContent(PlcSchemaStore& t_registry, const std::string& t_content,
        const std::string& t_source_name = "embedded", bool t_force = false,
        std::map<std::string, UdtDefinition>* tp_global_udts = nullptr);
};

} // namespace sgrn::scl
