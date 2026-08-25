#include <fmt/core.h>
#include <sgrn/scl/schema/DbSymbolsParser.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/schema/SclCompiler.hpp>
#include <sgrn/scl/schema/TagTable.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/scl/utils.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

namespace sgrn::scl
{

namespace fs = std::filesystem;

struct SymbolFileInfo {
    uint16_t number{0};
    std::string ergonomic_name;
};

static SymbolFileInfo extractInfoFromFilename(const std::string& t_filepath, const std::regex& t_pattern) {
    const std::string t_filename = fs::path(t_filepath).filename().string();
    std::smatch m;
    if (std::regex_match(t_filename, m, t_pattern)) {
        SymbolFileInfo info;
        info.number = static_cast<uint16_t>(sgrn::utils::strings::parseInt(m[1].str()).value_or(0));
        if (m[2].matched)
            info.ergonomic_name = m[2].str();
        return info;
    }
    return {};
}

static int extractTrailingNumber(const std::string& t_value) {
    auto it = t_value.rbegin();
    while (it != t_value.rend() && *it == ' ')
        ++it;
    if (it == t_value.rend() || !std::isdigit(static_cast<unsigned char>(*it)))
        return 0;
    auto digit_end = it.base();
    while (it != t_value.rend() && std::isdigit(static_cast<unsigned char>(*it)))
        ++it;
    return sgrn::utils::strings::parseInt(std::string(it.base(), digit_end)).value_or(0);
}

static const std::regex kDbFilenameRe(R"(^DB(\d+)(?:-([^.]+))?\.\w+$)", std::regex::icase);
static const std::regex kUdtFilenameRe(R"(^UDT(\d+)(?:-([^.]+))?\.\w+$)", std::regex::icase);

static bool isDbOrUdtTextSymbolFile(const std::string& t_filepath) {
    std::string ext = fs::path(t_filepath).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".scl" || ext == ".db" || ext == ".stl" || ext == ".awl";
}

static bool isTagTableXmlFile(const std::string& t_filepath) {
    std::string ext = fs::path(t_filepath).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".xml";
}

static bool contentLooksLikeJson(const std::string& t_filepath) {
    std::ifstream file(t_filepath);
    if (!file)
        return false;
    char ch;
    while (file.get(ch)) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return (ch == '{' || ch == '[');
        }
    }
    return false;
}

static Result<void, ::sgrn::scl::Error> mergeIntoRegistry(PlcSchemaStore& t_registry, ParseResult&& t_result,
    const std::string& t_source_name, bool t_force, const SymbolFileInfo& t_db_info = {}, const SymbolFileInfo& t_udt_info = {}) {

    for (const std::string& w : t_result.warnings) {
        t_registry.addWarning(fmt::format("[{}] {}", t_source_name, w));
    }

    for (UdtDefinition& t_udt : t_result.udts) {
        if (t_udt.udt_number == 0 && t_udt_info.number > 0) {
            t_udt.udt_number = t_udt_info.number;
        }
        if (t_udt.name.empty() && !t_udt_info.ergonomic_name.empty()) {
            t_udt.name = t_udt_info.ergonomic_name;
        }

        Result<void, ::sgrn::scl::Error> r = t_registry.addUdt(std::move(t_udt), t_force, false);
        if (r.hasError())
            return Error(r.error());
    }

    for (DbSchema& t_db : t_result.dbs) {
        if (t_db.source_file.empty()) {
            t_db.source_file = t_source_name;
        }

        if (t_db.db_number <= 0) {
            t_db.db_number = t_db_info.number;
        }
        if (t_db.db_number <= 0) {
            t_db.db_number = extractTrailingNumber(t_db.db_name);
        }

        if (t_db.db_name.empty() && !t_db_info.ergonomic_name.empty()) {
            t_db.db_name = t_db_info.ergonomic_name;
        }

        if (t_db.db_number <= 0) {
            int auto_db_counter = 1;
            while (t_registry.hasDb(auto_db_counter))
                auto_db_counter++;
            t_db.db_number = auto_db_counter;
            t_registry.addWarning(
                fmt::format("[{}] auto-assigned DB number {} for block '{}'", t_source_name, t_db.db_number, t_db.db_name));
        }

        Result<void, ::sgrn::scl::Error> r = t_registry.addDb(std::move(t_db), t_force, false);
        if (r.hasError())
            return Error(r.error());
    }

    return {};
}

Result<UdtDefinition, ::sgrn::scl::Error> SclCompiler::parseUdtFile(
    const std::string& t_path, std::map<std::string, UdtDefinition>* tp_global_udts) {
    auto res = DbSymbolsParser::parseExportFile(t_path, tp_global_udts);
    if (res.hasError())
        return res.error();
    if (res.value().udts.empty())
        return Error{SchemaCode::Generic, "no UDT found in file"};
    return std::move(res.value().udts[0]);
}

Result<DbSchema, ::sgrn::scl::Error> SclCompiler::parseDbFile(
    const std::string& t_path, std::map<std::string, UdtDefinition>* tp_global_udts) {
    auto res = DbSymbolsParser::parseExportFile(t_path, tp_global_udts);
    if (res.hasError())
        return res.error();
    if (res.value().dbs.empty())
        return Error{SchemaCode::Generic, "no DB found in file"};
    return std::move(res.value().dbs[0]);
}

Result<std::vector<PlcTag>, ::sgrn::scl::Error> SclCompiler::parseTagTableXmlFile(const std::string& t_path) {
    std::vector<std::string> dummy;
    return ::sgrn::scl::parseTagTableXmlFile(t_path, dummy);
}

Result<void, ::sgrn::scl::Error> SclCompiler::loadFromDirectory(
    PlcSchemaStore& t_registry, const std::string& t_symbols_dir, bool t_force) {
    std::error_code ec;
    if (!fs::is_directory(t_symbols_dir, ec))
        return Err::Generic("symbols directory does not exist: {}", t_symbols_dir);
    t_registry.base_dir_ = t_symbols_dir;
    std::vector<std::string> text_files;
    std::vector<std::string> other_files;

    for (const auto& entry : fs::directory_iterator(t_symbols_dir)) {
        if (!entry.is_regular_file())
            continue;
        std::string t_path = entry.path().string();
        if (isDbOrUdtTextSymbolFile(t_path))
            text_files.push_back(t_path);
        else if (isTagTableXmlFile(t_path) || contentLooksLikeJson(t_path))
            other_files.push_back(t_path);
    }

    // Two-pass resolution: TIA Portal exports one file per UDT/DB, with no
    // guaranteed ordering — a DB can reference a UDT defined in a file that
    // sorts alphabetically after it. A single linear pass would silently
    // fail to resolve forward references.
    //
    // Instead we re-parse the whole text_files set repeatedly, feeding
    // discovered UDTs back into global_udts, until a full pass adds nothing
    // new (fixed point). This makes loading order-independent regardless of
    // how the user's export directory is organized, at the cost of O(n^2)
    // parsing on the UDT set — acceptable since schema sets are small
    // (tens to low hundreds of files) and this only runs at load time.
    std::map<std::string, UdtDefinition> t_global_udts;
    int prev_count = -1;
    std::set<std::string> warned_first_pass;
    while (static_cast<int>(t_global_udts.size()) != prev_count) {
        prev_count = static_cast<int>(t_global_udts.size());
        for (const auto& t_path : text_files) {
            auto pr = DbSymbolsParser::parseExportFile(t_path, &t_global_udts);
            if (pr.hasError() && warned_first_pass.insert(t_path).second) {
                t_registry.addWarning(fmt::format("[{}] parse error (UDT discovery): {}", t_path, pr.error().string()));
            }
        }
    }

    // Second pass: now that global_udts is fully resolved, actually register
    // each DB/UDT into the schema store (this is where addDb/addUdt errors,
    // like duplicate DB numbers, get surfaced to the caller).
    for (const auto& t_path : text_files) {
        auto r = loadFile(t_registry, t_path, t_force, &t_global_udts);
        if (r.hasError())
            return Error(r.error());
    }
    return {};
}

Result<void, ::sgrn::scl::Error> SclCompiler::loadFromFiles(
    PlcSchemaStore& t_registry, const std::vector<std::string>& t_filepaths, bool t_force) {
    std::map<std::string, UdtDefinition> t_global_udts;
    int prev_count = -1;
    std::set<std::string> warned_first_pass;
    while (static_cast<int>(t_global_udts.size()) != prev_count) {
        prev_count = static_cast<int>(t_global_udts.size());
        for (const auto& t_path : t_filepaths) {
            if (isDbOrUdtTextSymbolFile(t_path)) {
                auto pr = DbSymbolsParser::parseExportFile(t_path, &t_global_udts);
                if (pr.hasError() && warned_first_pass.insert(t_path).second) {
                    t_registry.addWarning(fmt::format("[{}] parse error (UDT discovery): {}", t_path, pr.error().string()));
                }
            }
        }
    }

    for (const auto& t_path : t_filepaths) {
        auto r = loadFile(t_registry, t_path, t_force, &t_global_udts);
        if (r.hasError())
            return Error(r.error());
    }
    return {};
}

Result<void, ::sgrn::scl::Error> SclCompiler::loadFile(
    PlcSchemaStore& t_registry, const std::string& t_filepath, bool t_force, std::map<std::string, UdtDefinition>* tp_global_udts) {

    if (isTagTableXmlFile(t_filepath)) {
        std::vector<std::string> warnings;
        auto tags = ::sgrn::scl::parseTagTableXmlFile(t_filepath, warnings);
        for (auto& tag : tags) {
            t_registry.addTag(std::move(tag), t_force);
        }
        for (const auto& msg : warnings) {
            t_registry.addWarning(fmt::format("[{}] {}", t_filepath, msg));
        }
        return {};
    }

    std::ifstream file(t_filepath);
    if (!file.is_open())
        return Err::FileNotFound("cannot open symbol file: {}", t_filepath);

    std::string t_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    SymbolFileInfo t_db_info = extractInfoFromFilename(t_filepath, kDbFilenameRe);
    SymbolFileInfo t_udt_info = extractInfoFromFilename(t_filepath, kUdtFilenameRe);

    auto res = loadFromContent(t_registry, t_content, t_filepath, t_force, tp_global_udts);
    if (res.hasError())
        return res;

    return {};
}

Result<void, ::sgrn::scl::Error> SclCompiler::loadFromContent(PlcSchemaStore& t_registry, const std::string& t_content,
    const std::string& t_source_name, bool t_force, std::map<std::string, UdtDefinition>* tp_global_udts) {

    auto first_char = [](const std::string& t_s) {
        for (char c : t_s)
            if (!std::isspace(static_cast<unsigned char>(c)))
                return c;
        return '\0';
    }(t_content);

    if (first_char == '{' || first_char == '[') {
        rapidjson::Document root;
        root.Parse(t_content.c_str());
        if (root.HasParseError())
            return Err::ParseError("failed to parse JSON schema from {}", t_source_name);

        Result<PlcSchemaStore, ::sgrn::scl::Error> nested = PlcSchemaStore::loadFromJson(root);
        if (nested.hasError())
            return Error(nested.error());

        ParseResult pr;
        for (auto& [_, t_db] : nested.value().dbs_)
            pr.dbs.push_back(std::move(t_db));
        for (auto& t_udt : nested.value().udts_)
            pr.udts.push_back(std::move(t_udt));

        auto res = mergeIntoRegistry(t_registry, std::move(pr), t_source_name, t_force);
        if (res.hasError())
            return res;

        if (root.HasMember("tags") && root["tags"].IsArray()) {
            for (const auto& tag_node : root["tags"].GetArray()) {
                if (auto tag = SchemaSerializer::tagFromJson(tag_node)) {
                    t_registry.addTag(std::move(*tag), t_force);
                }
            }
        }
        t_registry.rebuildIndices();
        return {};
    }

    auto parse_result = DbSymbolsParser::parseString(t_content, tp_global_udts);
    if (parse_result.hasError())
        return Error(parse_result.error());

    SymbolFileInfo t_db_info = extractInfoFromFilename(t_source_name, kDbFilenameRe);
    SymbolFileInfo t_udt_info = extractInfoFromFilename(t_source_name, kUdtFilenameRe);

    auto res = mergeIntoRegistry(t_registry, std::move(parse_result.value()), t_source_name, t_force, t_db_info, t_udt_info);
    if (res.hasError())
        return res;

    t_registry.rebuildIndices();
    return {};
}
// ── Phase 2: High-level compile API ─────────────────────────────────────────

Result<PlcSchemaStore, SchemaError> SclCompiler::compileFile(const std::string& t_path, Options t_opts) {
    PlcSchemaStore t_store;
    auto res = loadFile(t_store, t_path, t_opts.force);
    if (res.hasError())
        return Error(res.error());
    return t_store;
}

Result<PlcSchemaStore, SchemaError> SclCompiler::compileDirectory(const std::string& t_dir, Options t_opts) {
    PlcSchemaStore t_store;
    auto res = loadFromDirectory(t_store, t_dir, t_opts.force);
    if (res.hasError())
        return Error(res.error());
    return t_store;
}

Result<PlcSchemaStore, SchemaError> SclCompiler::compileFiles(const std::vector<std::string>& t_paths, Options t_opts) {
    PlcSchemaStore t_store;
    auto res = loadFromFiles(t_store, t_paths, t_opts.force);
    if (res.hasError())
        return Error(res.error());
    return t_store;
}

// ── Phase 2: Emit outputs ───────────────────────────────────────────────────

Result<void, SchemaError> SclCompiler::emitJson(const PlcSchemaStore& t_store, const std::string& t_output_path, bool t_pretty) {
    return t_store.saveToJsonFile(t_output_path);
}

Result<void, SchemaError> SclCompiler::emitScl(const PlcSchemaStore& t_store, const std::string& t_output_dir) {
    std::error_code ec;
    fs::create_directories(t_output_dir, ec);
    if (ec)
        return Err::IoError("cannot create output directory '{}': {}", t_output_dir, ec.message());

    // Emit UDTs
    for (const auto& t_udt : t_store.udts()) {
        std::string t_filename = (t_udt.udt_number > 0) ? canonicalUdtFilename(t_udt.udt_number, t_udt.name)
                                                        : fmt::format("{}.udt", t_udt.name.empty() ? "unnamed" : t_udt.name);
        t_filename = fs::path(t_filename).replace_extension(".scl").string();

        std::string t_path = (fs::path(t_output_dir) / t_filename).string();
        std::ofstream t_out(t_path);
        if (!t_out.is_open())
            return Err::IoError("cannot create file: {}", t_path);
        t_out << udtToScl(t_udt);
    }

    // Emit DBs
    for (uint16_t db_num : t_store.availableDbs()) {
        auto db_res = t_store.getDb(db_num);
        if (db_res.hasError())
            continue;
        const auto& t_db = *db_res.value();

        std::string t_filename = canonicalDbFilename(t_db.db_number, t_db.db_name);
        t_filename = fs::path(t_filename).replace_extension(".scl").string();

        std::string t_path = (fs::path(t_output_dir) / t_filename).string();
        std::ofstream t_out(t_path);
        if (!t_out.is_open())
            return Err::IoError("cannot create file: {}", t_path);
        t_out << dbToScl(t_db);
    }

    return {};
}

Result<void, SchemaError> SclCompiler::emitCanonical(const PlcSchemaStore& t_store, const std::string& t_output_dir) {
    std::error_code ec;
    fs::create_directories(t_output_dir, ec);
    if (ec)
        return Err::IoError("cannot create output directory '{}': {}", t_output_dir, ec.message());

    // Emit UDTs with canonical names
    for (const auto& t_udt : t_store.udts()) {
        if (t_udt.udt_number == 0)
            continue;
        std::string t_filename = canonicalUdtFilename(t_udt.udt_number, t_udt.name);
        std::string t_path = (fs::path(t_output_dir) / t_filename).string();
        std::ofstream t_out(t_path);
        if (!t_out.is_open())
            return Err::IoError("cannot create file: {}", t_path);
        t_out << udtToScl(t_udt);
    }

    // Emit DBs with canonical names
    for (uint16_t db_num : t_store.availableDbs()) {
        auto db_res = t_store.getDb(db_num);
        if (db_res.hasError())
            continue;
        const auto& t_db = *db_res.value();

        std::string t_filename = canonicalDbFilename(t_db.db_number, t_db.db_name);
        std::string t_path = (fs::path(t_output_dir) / t_filename).string();
        std::ofstream t_out(t_path);
        if (!t_out.is_open())
            return Err::IoError("cannot create file: {}", t_path);
        t_out << dbToScl(t_db);
    }

    // Emit JSON registry alongside
    std::string json_path = (fs::path(t_output_dir) / "registry.json").string();
    auto json_res = emitJson(t_store, json_path, true);
    if (json_res.hasError())
        return json_res;

    return {};
}

// ── Phase 2: Canonical naming ───────────────────────────────────────────────

std::string SclCompiler::canonicalUdtFilename(uint16_t t_index, const std::string& t_name) {
    if (t_name.empty())
        return fmt::format("UDT{}.udt", t_index);
    return fmt::format("UDT{}-{}.udt", t_index, t_name);
}

std::string SclCompiler::canonicalDbFilename(uint16_t t_index, const std::string& t_name) {
    if (t_name.empty())
        return fmt::format("DB{}.db", t_index);
    return fmt::format("DB{}-{}.db", t_index, t_name);
}

std::optional<std::pair<uint16_t, std::string>> SclCompiler::parseCanonicalFilename(const std::string& t_filename) {
    // Match: DB{num}-{name}.ext or UDT{num}-{name}.ext
    static const std::regex kCanonicalRe(R"(^(?:DB|UDT)(\d+)(?:-([^.]+))?\.\w+$)", std::regex::icase);
    std::string basename = fs::path(t_filename).filename().string();
    std::smatch m;
    if (!std::regex_match(basename, m, kCanonicalRe))
        return std::nullopt;

    uint16_t t_index = static_cast<uint16_t>(sgrn::utils::strings::parseInt(m[1].str()).value_or(0));
    std::string t_name = m[2].matched ? m[2].str() : "";
    return std::pair{t_index, t_name};
}

// ── Phase 2: SCL text generation ────────────────────────────────────────────

static void emitFieldsScl(std::string& t_out, const std::vector<DbField>& t_fields, int t_indent) {
    std::string pad(t_indent * 4, ' ');
    for (const auto& t_f : t_fields) {
        if (t_f.type == DataType::Struct) {
            if (t_f.count > 1) {
                // Array of struct
                t_out += fmt::format("{}{} : Array[0..{}] of Struct\n", pad, t_f.name, t_f.count - 1);
            } else if (!t_f.udt_name.empty()) {
                // UDT reference
                t_out += fmt::format("{}{} : \"{}\";\n", pad, t_f.name, t_f.udt_name);
                continue;
            } else {
                t_out += fmt::format("{}{} : Struct\n", pad, t_f.name);
            }
            emitFieldsScl(t_out, t_f.children, t_indent + 1);
            t_out += fmt::format("{}END_STRUCT;\n", pad);
        } else {
            std::string type_str = s7codec::s7TypeToString(t_f.type);
            if (t_f.type == DataType::String && t_f.count > 0) {
                type_str = fmt::format("String[{}]", t_f.count);
            } else if (t_f.type == DataType::WString && t_f.count > 0) {
                type_str = fmt::format("WString[{}]", t_f.count);
            } else if (t_f.count > 1 && t_f.type != DataType::Bool) {
                // Array of primitive
                t_out += fmt::format("{}{} : Array[0..{}] of {};\n", pad, t_f.name, t_f.count - 1, type_str);
                continue;
            }
            t_out += fmt::format("{}{} : {};\n", pad, t_f.name, type_str);
        }
    }
}

std::string SclCompiler::udtToScl(const UdtDefinition& t_udt) {
    std::string t_out;
    t_out += fmt::format("TYPE \"{}\"\n", t_udt.name);
    t_out += "VERSION : 0.1\n\n";
    t_out += "  STRUCT\n";
    emitFieldsScl(t_out, t_udt.fields, 2);
    t_out += "  END_STRUCT;\n\n";
    t_out += "END_TYPE\n";
    return t_out;
}

std::string SclCompiler::dbToScl(const DbSchema& t_db) {
    std::string t_out;
    t_out += fmt::format("DATA_BLOCK \"{}\"\n", t_db.db_name.empty() ? fmt::format("DB{}", t_db.db_number) : t_db.db_name);
    t_out += fmt::format("{{ S7_Optimized_Access := 'FALSE' }}\n");
    t_out += "VERSION : 0.1\n";
    t_out += fmt::format("NON_RETAIN\n\n");

    t_out += "  STRUCT\n";
    emitFieldsScl(t_out, t_db.fields, 2);
    t_out += "  END_STRUCT;\n\n";

    t_out += "BEGIN\n\n";
    t_out += "END_DATA_BLOCK\n";
    return t_out;
}

// ── Phase 2: C++ code generation (s7codec compatible) ───────────────────────

/// Map a DbField type to its s7codec C++ type name
static std::string fieldToCppType(const DbField& t_f) {
    using T = DataType;
    switch (t_f.type) {
        case T::Bool:
            return "s7codec::Bool";
        case T::Byte:
            return "s7codec::Byte";
        case T::USInt:
            return "s7codec::USInt";
        case T::SInt:
            return "s7codec::SInt";
        case T::Char:
            return "s7codec::Char";
        case T::Word:
            return "s7codec::Word";
        case T::UInt:
            return "s7codec::UInt";
        case T::Int:
            return "s7codec::Int";
        case T::WChar:
            return "s7codec::WChar";
        case T::DWord:
            return "s7codec::DWord";
        case T::UDInt:
            return "s7codec::UDInt";
        case T::DInt:
            return "s7codec::DInt";
        case T::Real:
            return "s7codec::Real";
        case T::Time:
            return "s7codec::Time";
        case T::LWord:
            return "s7codec::LWord";
        case T::ULInt:
            return "s7codec::ULInt";
        case T::LInt:
            return "s7codec::LInt";
        case T::LReal:
            return "s7codec::LReal";
        case T::DTL:
            return "s7codec::DTL";
        case T::DateTime:
            return "s7codec::DateTime"; // 8-byte BCD date_and_time
        case T::Date:
            return "s7codec::Word"; // 16-bit days since epoch
        case T::String: {
            // For scalar strings: capacity is the max length, count is 1
            // For array of strings: capacity is the max length per element, count is array size
            int cap = 254;
            if (t_f.struct_size > 0 && t_f.count > 1) {
                // Array of strings: struct_size holds per-element capacity
                cap = t_f.struct_size;
            } else if (t_f.count > 0) {
                // Scalar string: count IS the capacity (legacy encoding)
                cap = t_f.count;
            }
            return fmt::format("s7codec::S7RawString<{}>", cap);
        }
        case T::WString: {
            int cap = 254;
            if (t_f.struct_size > 0 && t_f.count > 1) {
                cap = t_f.struct_size;
            } else if (t_f.count > 0) {
                cap = t_f.count;
            }
            return fmt::format("s7codec::S7RawWString<{}>", cap);
        }
        case T::XString:
            // XString has 8-byte header — treat as raw bytes for now
            return fmt::format("uint8_t /* XString[{}] */", t_f.count > 0 ? t_f.count : 1024);
        case T::XWString:
            return fmt::format("uint8_t /* XWString[{}] */", t_f.count > 0 ? t_f.count : 1024);
        case T::Struct:
            return "/* struct */";
        default:
            return "uint8_t /* unknown */";
    }
}

/// Sanitize a name to be a valid C++ identifier
static std::string sanitizeIdentifier(const std::string& t_name) {
    std::string t_out;
    t_out.reserve(t_name.size());
    for (char c : t_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            t_out += c;
        else
            t_out += '_';
    }
    if (!t_out.empty() && std::isdigit(static_cast<unsigned char>(t_out[0])))
        t_out = "_" + t_out;
    return t_out;
}

/// Check if a field is a Bool (for bit packing)
static bool isBoolField(const DbField& t_f) {
    return t_f.type == DataType::Bool && t_f.count <= 1;
}

/// Check if a run of consecutive fields are all Bools (for S7_BIT_GROUP)
static size_t countConsecutiveBools(const std::vector<DbField>& t_fields, size_t t_start) {
    size_t count = 0;
    for (size_t i = t_start; i < t_fields.size(); ++i) {
        if (isBoolField(t_fields[i]))
            ++count;
        else
            break;
    }
    return count;
}

/// Emit fields as C++ struct members, handling bit packing and composition
static void emitFieldsCpp(std::string& t_out, const std::vector<DbField>& t_fields, int t_indent) {
    std::string pad(t_indent * 4, ' ');

    for (size_t i = 0; i < t_fields.size();) {
        const auto& t_f = t_fields[i];

        // ── Bit packing: consecutive Bools → S7_BIT_GROUP ────────────
        //
        // S7 addresses bools at the bit level (byte.bit, e.g. DB1.DBX0.3),
        // not the byte level like every other type here. TIA Portal packs
        // up to 8 consecutive bool fields into a single byte in the real
        // datablock layout. If we emitted one s7codec::Bool per field
        // unconditionally, every run of 2+ bools would desync our C++
        // struct's byte offsets from the PLC's actual memory layout —
        // the exact bug this whole schema-driven approach exists to
        // eliminate. So a run of >=2 consecutive bools is emitted as a
        // single S7_BIT_GROUP, which packs them at the bit level to match
        // the PLC; a lone bool is emitted as a normal byte-aligned
        // s7codec::Bool since there's nothing to pack it with.
        if (isBoolField(t_f)) {
            size_t bool_run = countConsecutiveBools(t_fields, i);
            if (bool_run >= 2) {
                t_out += fmt::format("{}S7_BIT_GROUP_START\n", pad);
                for (size_t j = 0; j < bool_run; ++j) {
                    t_out += fmt::format("{}    S7_BIT({});\n", pad, sanitizeIdentifier(t_fields[i + j].name));
                }
                t_out += fmt::format("{}S7_BIT_GROUP_END;\n", pad);
                i += bool_run;
                continue;
            }
            // Single, unpackable bool — byte-aligned, not bit-packed.
            t_out += fmt::format("{}s7codec::Bool {};\n", pad, sanitizeIdentifier(t_f.name));
            ++i;
            continue;
        }
        // ── Struct / UDT composition ─────────────────────────────────
        if (t_f.type == DataType::Struct) {
            std::string struct_name = sanitizeIdentifier(t_f.udt_name.empty() ? t_f.name : t_f.udt_name);

            if (t_f.count > 1) {
                // Array of struct
                t_out += fmt::format("{}struct {} {{\n", pad, struct_name);
                emitFieldsCpp(t_out, t_f.children, t_indent + 1);
                t_out += fmt::format("{}}};\n", pad);
                t_out += fmt::format("{}{} {}[{}];\n", pad, struct_name, sanitizeIdentifier(t_f.name), t_f.count);
            } else {
                // Inline struct or UDT reference
                t_out += fmt::format("{}struct {{ // {}\n", pad, t_f.udt_name.empty() ? t_f.name : fmt::format("UDT \"{}\"", t_f.udt_name));
                emitFieldsCpp(t_out, t_f.children, t_indent + 1);
                t_out += fmt::format("{}}} {};\n", pad, sanitizeIdentifier(t_f.name));
            }
            ++i;
            continue;
        }

        // ── String/WString ────────────────────────────────────────────
        if (t_f.type == DataType::String || t_f.type == DataType::WString) {
            if (t_f.struct_size > 0 && t_f.count > 1) {
                // Array of strings: emit as C array
                t_out += fmt::format("{}{} {}[{}];\n", pad, fieldToCppType(t_f), sanitizeIdentifier(t_f.name), t_f.count);
            } else {
                t_out += fmt::format("{}{} {};\n", pad, fieldToCppType(t_f), sanitizeIdentifier(t_f.name));
            }
            ++i;
            continue;
        }

        // ── XString (raw byte array with header) ─────────────────────
        if (t_f.type == DataType::XString || t_f.type == DataType::XWString) {
            int span = s7codec::typeSpanBytes(t_f.type, t_f.count);
            t_out += fmt::format("{}uint8_t {}[{}]; // {}\n", pad, sanitizeIdentifier(t_f.name), span,
                t_f.type == DataType::XString ? fmt::format("XString[{}]", t_f.count) : fmt::format("XWString[{}]", t_f.count));
            ++i;
            continue;
        }

        // ── Array of primitives ──────────────────────────────────────
        if (t_f.count > 1 && t_f.type != DataType::Bool) {
            t_out += fmt::format("{}{} {}[{}];\n", pad, fieldToCppType(t_f), sanitizeIdentifier(t_f.name), t_f.count);
            ++i;
            continue;
        }

        // ── Array of bools (packed) ──────────────────────────────────
        if (t_f.count > 1 && t_f.type == DataType::Bool) {
            int bytes = (t_f.count + 7) / 8;
            t_out += fmt::format("{}uint8_t {}[{}]; // {} packed bools\n", pad, sanitizeIdentifier(t_f.name), bytes, t_f.count);
            ++i;
            continue;
        }

        // ── Scalar primitive ─────────────────────────────────────────
        t_out += fmt::format("{}{} {};\n", pad, fieldToCppType(t_f), sanitizeIdentifier(t_f.name));
        ++i;
    }
}

std::string SclCompiler::emitCppHeader(const PlcSchemaStore& t_store, const std::string& t_guard_prefix) {
    std::string t_out;

    // Header guard
    std::string guard = sanitizeIdentifier(t_guard_prefix) + "_HPP";
    for (auto& c : guard)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    t_out += fmt::format("// Auto-generated by sclc — do not edit.\n");
    t_out += fmt::format("// Regenerate with: sclc codegen --parse <schema> -o <output.hpp>\n");
    t_out += fmt::format("#pragma once\n");
    t_out += fmt::format("#ifndef {}\n", guard);
    t_out += fmt::format("#define {}\n\n", guard);
    t_out += fmt::format("#include <s7codec/s7.hpp>\n\n");
    // S7 PLCs use word-alignment (2 bytes) for all fields.
    // Without this, C++ natural alignment (4/8) breaks struct layouts.
    t_out += "#pragma pack(push, 2)\n\n";

    // Emit UDTs as standalone structs first (dependency order)
    if (!t_store.udts().empty()) {
        t_out += "// ── UDT Definitions ─────────────────────────────────────────────────\n\n";
        for (const auto& t_udt : t_store.udts()) {
            std::string t_name = sanitizeIdentifier(t_udt.name);
            t_out += fmt::format("struct {} {{ // UDT{} — {} bytes\n", t_name, t_udt.udt_number, t_udt.size_bytes);
            emitFieldsCpp(t_out, t_udt.fields, 1);
            t_out += fmt::format("}};\n");
            t_out += fmt::format("static_assert(sizeof({}) == {}, \"UDT {} size mismatch\");\n\n", t_name, t_udt.size_bytes, t_name);
        }
    }

    // Emit DBs as DATABLOCK structs
    if (!t_store.availableDbs().empty()) {
        t_out += "// ── Data Blocks ─────────────────────────────────────────────────────\n\n";
        for (uint16_t db_num : t_store.availableDbs()) {
            auto db_res = t_store.getDb(db_num);
            if (db_res.hasError())
                continue;
            const auto& t_db = *db_res.value();

            std::string t_name = sanitizeIdentifier(t_db.db_name.empty() ? fmt::format("DB{}", t_db.db_number) : t_db.db_name);

            t_out += fmt::format("DATABLOCK({}) {{ // DB{} — {} bytes\n", t_name, t_db.db_number, t_db.size_bytes);
            emitFieldsCpp(t_out, t_db.fields, 1);
            t_out += fmt::format("}};\n");
            t_out += fmt::format("static_assert(sizeof({}) == {}, \"DB{} size mismatch\");\n\n", t_name, t_db.size_bytes, t_db.db_number);
        }
    }

    t_out += "#pragma pack(pop)\n\n";
    t_out += fmt::format("#endif // {}\n", guard);
    return t_out;
}

Result<void, SchemaError> SclCompiler::emitCpp(
    const PlcSchemaStore& t_store, const std::string& t_output_path, const std::string& t_guard_prefix) {
    std::string t_content = emitCppHeader(t_store, t_guard_prefix);

    std::error_code ec;
    fs::path parent = fs::path(t_output_path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec)
            return Err::IoError("cannot create directory '{}': {}", parent.string(), ec.message());
    }

    std::ofstream t_out(t_output_path);
    if (!t_out.is_open())
        return Err::IoError("cannot create file: {}", t_output_path);
    t_out << t_content;
    return {};
}

} // namespace sgrn::scl
