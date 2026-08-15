#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/schema/SclCompiler.hpp>
#include <sgrn/scl/utils.hpp>

#include <fmt/core.h>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace sgrn::scl
{

namespace fs = std::filesystem;

static sgrn::FieldDefinition toCoreField(const DbField& t_f) {
    sgrn::FieldDefinition core;
    core.name = t_f.name;
    core.offset = t_f.offset;
    core.size = s7codec::typeSpanBytes(t_f.type, t_f.count);
    core.bit_index = t_f.bit_index;
    core.count = t_f.count;
    core.template_name = t_f.udt_name;
    for (const auto& child : t_f.children) {
        core.children.push_back(toCoreField(child));
    }
    core.context["s7_type"] = static_cast<int>(t_f.type);
    return core;
}

static sgrn::BlockDefinition toCoreBlock(const DbSchema& t_db) {
    sgrn::BlockDefinition core;
    core.name = t_db.db_name;
    core.id = t_db.db_number;
    core.size = t_db.size_bytes;
    for (const auto& t_f : t_db.fields) {
        core.fields.push_back(toCoreField(t_f));
    }
    core.context["db_number"] = t_db.db_number;
    return core;
}

static sgrn::SchemaTemplate toCoreTemplate(const UdtDefinition& t_udt) {
    sgrn::SchemaTemplate core;
    core.name = t_udt.name;
    core.size = t_udt.size_bytes;
    for (const auto& t_f : t_udt.fields) {
        core.fields.push_back(toCoreField(t_f));
    }
    core.context["udt_number"] = t_udt.udt_number;
    return core;
}

// ── Index Rebuilding ────────────────────────────────────────────────────────

void PlcSchemaStore::rebuildIndices() {
    dbs_by_name_.clear();
    for (auto& [num, t_db] : dbs_) {
        if (!t_db.db_name.empty())
            dbs_by_name_[t_db.db_name] = &t_db;
    }

    udts_by_name_.clear();
    udts_by_number_.clear();
    for (const auto& t_udt : udts_) {
        if (!t_udt.name.empty())
            udts_by_name_[t_udt.name] = &t_udt;
        if (t_udt.udt_number > 0)
            udts_by_number_[t_udt.udt_number] = &t_udt;
    }
}

// ── Factory Methods ─────────────────────────────────────────────────────────

sgrn::Result<PlcSchemaStore, ::sgrn::scl::Error> PlcSchemaStore::loadFromDirectory(const std::string& t_symbols_dir, bool t_force) {
    PlcSchemaStore store;
    auto res = SclCompiler::loadFromDirectory(store, t_symbols_dir, t_force);
    if (res.hasError())
        return Error(res.error());
    return store;
}

sgrn::Result<PlcSchemaStore, ::sgrn::scl::Error> PlcSchemaStore::loadFromFile(const std::string& t_filepath, bool t_force) {
    return loadFromFiles({t_filepath}, t_force);
}

sgrn::Result<PlcSchemaStore, ::sgrn::scl::Error> PlcSchemaStore::loadFromFiles(const std::vector<std::string>& t_filepaths, bool t_force) {
    PlcSchemaStore store;
    auto res = SclCompiler::loadFromFiles(store, t_filepaths, t_force);
    if (res.hasError())
        return Error(res.error());
    return store;
}

sgrn::Result<PlcSchemaStore, ::sgrn::scl::Error> PlcSchemaStore::loadFromJsonFile(const std::string& t_path) {
    std::ifstream file(t_path);
    if (!file.is_open())
        return Err::FileNotFound("cannot open symbol JSON file: {}", t_path);

    std::string json_str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    rapidjson::Document t_root;
    t_root.Parse(json_str.c_str());
    if (t_root.HasParseError())
        return Err::ParseError("failed to parse symbol JSON '{}': offset {}", t_path, t_root.GetErrorOffset());

    sgrn::Result<PlcSchemaStore, ::sgrn::scl::Error> store_res = loadFromJson(t_root);
    if (store_res.hasError())
        return Error(store_res.error());

    store_res->base_dir_ = fs::path(t_path).parent_path().string();
    return store_res;
}

sgrn::Result<PlcSchemaStore, ::sgrn::scl::Error> PlcSchemaStore::loadFromJson(const rapidjson::Value& t_root) {
    PlcSchemaStore store;
    auto res = SchemaSerializer::deserialize(store, t_root);
    if (res.hasError())
        return Error(res.error());
    return store;
}

// ── DB Schema Queries ───────────────────────────────────────────────────────

bool PlcSchemaStore::hasDb(uint16_t t_db_number) const {
    return dbs_.count(t_db_number) > 0;
}

bool PlcSchemaStore::hasDb(const std::string& t_db_name) const {
    return dbs_by_name_.count(t_db_name) > 0;
}

sgrn::Result<const DbSchema*, ::sgrn::scl::Error> PlcSchemaStore::getDb(uint16_t t_db_number) const {
    auto it = dbs_.find(t_db_number);
    if (it == dbs_.end())
        return Err::NotFound("DB{} has no symbol file loaded", t_db_number);
    return &it->second;
}

sgrn::Result<const DbSchema*, ::sgrn::scl::Error> PlcSchemaStore::getDbByName(const std::string& t_db_name) const {
    auto it = dbs_by_name_.find(t_db_name);
    if (it == dbs_by_name_.end())
        return Err::NotFound("DB '{}' has no symbol file loaded", t_db_name);
    return it->second;
}

// ── Field Lookup ────────────────────────────────────────────────────────────

std::optional<FieldLocation> PlcSchemaStore::findField(uint16_t t_db_number, std::string_view t_field_path) const {
    auto res = getDb(t_db_number);
    if (res.hasError())
        return std::nullopt;
    const auto* p_db = res.value();

    auto loc = ::sgrn::scl::findFieldByPath(p_db->fields, std::string(t_field_path));
    if (!loc.has_value())
        return std::nullopt;

    return FieldLocation{t_db_number, p_db->db_name, loc->field, loc->abs_offset};
}

std::optional<FieldLocation> PlcSchemaStore::findField(std::string_view t_db_name, std::string_view t_field_path) const {
    auto res = getDbByName(std::string(t_db_name));
    if (res.hasError())
        return std::nullopt;
    const auto* p_db = res.value();

    auto loc = ::sgrn::scl::findFieldByPath(p_db->fields, std::string(t_field_path));
    if (!loc.has_value())
        return std::nullopt;

    return FieldLocation{p_db->db_number, p_db->db_name, loc->field, loc->abs_offset};
}

std::optional<uint16_t> PlcSchemaStore::resolveDbRef(std::string_view t_token) const {
    std::string s = std::string(t_token);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s.empty())
        return std::nullopt;

    if (s.size() > 2 && s[0] == 'd' && s[1] == 'b') {
        try {
            int num = std::stoi(s.substr(2));
            if (num > 0 && num <= 65535 && hasDb(static_cast<uint16_t>(num)))
                return static_cast<uint16_t>(num);
        } catch (...) {
        }
    }

    try {
        int num = std::stoi(s);
        if (num > 0 && num <= 65535 && hasDb(static_cast<uint16_t>(num)))
            return static_cast<uint16_t>(num);
    } catch (...) {
    }

    auto it = dbs_by_name_.find(std::string(t_token));
    if (it != dbs_by_name_.end())
        return it->second->db_number;

    return std::nullopt;
}

std::optional<FieldTarget> PlcSchemaStore::parseFieldTarget(std::string_view t_path) const {
    std::string p(t_path);
    auto dot = p.find('.');
    if (dot == std::string::npos)
        return std::nullopt;

    std::string db_part = p.substr(0, dot);
    std::string field_part = p.substr(dot + 1);

    auto db_opt = resolveDbRef(db_part);
    if (!db_opt)
        return std::nullopt;

    return FieldTarget{*db_opt, field_part};
}

// ── UDTs ────────────────────────────────────────────────────────────────────

const std::deque<UdtDefinition>& PlcSchemaStore::udts() const {
    return udts_;
}

bool PlcSchemaStore::hasUdt(uint16_t t_udt_number) const {
    return udts_by_number_.count(t_udt_number) > 0;
}

bool PlcSchemaStore::hasUdt(const std::string& t_udt_name) const {
    return udts_by_name_.count(t_udt_name) > 0;
}

sgrn::Result<const UdtDefinition*, ::sgrn::scl::Error> PlcSchemaStore::getUdt(uint16_t t_udt_number) const {
    auto it = udts_by_number_.find(t_udt_number);
    if (it == udts_by_number_.end())
        return Err::NotFound("UDT{} has no symbol file loaded", t_udt_number);
    return it->second;
}

sgrn::Result<const UdtDefinition*, ::sgrn::scl::Error> PlcSchemaStore::getUdtByName(const std::string& t_udt_name) const {
    auto it = udts_by_name_.find(t_udt_name);
    if (it == udts_by_name_.end())
        return Err::NotFound("UDT '{}' has no symbol file loaded", t_udt_name);
    return it->second;
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::addDb(DbSchema&& t_reg, bool t_force, bool t_rebuild_index) {
    if (dbs_.count(t_reg.db_number)) {
        if (!t_force) {
            auto& p_existing = dbs_[t_reg.db_number];
            if (p_existing.db_name == t_reg.db_name)
                return {};
            return Err::Conflict("DB{} already exists", t_reg.db_number);
        }
        dbs_.erase(t_reg.db_number);
    }

    if (!t_reg.db_name.empty()) {
        auto it = dbs_by_name_.find(t_reg.db_name);
        if (it != dbs_by_name_.end()) {
            if (!t_force)
                return Err::Conflict("Name '{}' already used by DB{}", t_reg.db_name, it->second->db_number);
            dbs_.erase(it->second->db_number);
        }
    }

    dbs_[t_reg.db_number] = std::move(t_reg);

    // Core update
    ::sgrn::SchemaStore::addBlock(toCoreBlock(dbs_[t_reg.db_number]));

    if (t_rebuild_index)
        rebuildIndices();
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::removeDb(uint16_t t_db_number) {
    if (dbs_.erase(t_db_number) == 0)
        return Err::NotFound("DB{} does not exist", t_db_number);
    rebuildIndices();
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::addUdt(UdtDefinition&& t_udt, bool t_force, bool t_rebuild_index) {
    const UdtDefinition* p_existing = nullptr;
    if (t_udt.udt_number > 0) {
        auto it = udts_by_number_.find(t_udt.udt_number);
        if (it != udts_by_number_.end())
            p_existing = it->second;
    }
    if (!p_existing && !t_udt.name.empty()) {
        auto it = udts_by_name_.find(t_udt.name);
        if (it != udts_by_name_.end())
            p_existing = it->second;
    }

    if (p_existing) {
        if (!t_force) {
            if (p_existing->udt_number == t_udt.udt_number && p_existing->name == t_udt.name)
                return {};
            if (p_existing->udt_number > 0 && t_udt.udt_number > 0 && p_existing->udt_number == t_udt.udt_number)
                return Err::Conflict("UDT {} already exists", t_udt.udt_number);
            return Err::Conflict("Name '{}' already exists", t_udt.name);
        }
        udts_.erase(std::remove_if(udts_.begin(), udts_.end(), [&](const UdtDefinition& u) { return &u == p_existing; }), udts_.end());
    }

    udts_.push_back(std::move(t_udt));

    // Core update
    ::sgrn::SchemaStore::addTemplate(toCoreTemplate(udts_.back()));

    if (t_rebuild_index)
        rebuildIndices();
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::removeUdt(uint16_t t_udt_number) {
    auto it = udts_by_number_.find(t_udt_number);
    if (it == udts_by_number_.end())
        return Err::NotFound("UDT{} does not exist", t_udt_number);

    const UdtDefinition* p_ptr = it->second;
    udts_.erase(std::remove_if(udts_.begin(), udts_.end(), [&](const UdtDefinition& u) { return &u == p_ptr; }), udts_.end());

    rebuildIndices();
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::addTag(PlcTag&& t_tag, bool t_force) {
    if (tags_.count(t_tag.name)) {
        if (!t_force)
            return Err::Conflict("Tag '{}' already exists", t_tag.name);
        tags_.erase(t_tag.name);
    }
    tags_[t_tag.name] = std::move(t_tag);
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::removeTag(const std::string& t_tag_name) {
    if (tags_.erase(t_tag_name) == 0)
        return Err::NotFound("Tag '{}' does not exist", t_tag_name);
    return {};
}

sgrn::Result<const PlcTag*, ::sgrn::scl::Error> PlcSchemaStore::getTag(const std::string& t_tag_name) const {
    auto it = tags_.find(t_tag_name);
    if (it == tags_.end())
        return Err::NotFound("Tag '{}' not found", t_tag_name);
    return &it->second;
}

const std::map<std::string, PlcTag>& PlcSchemaStore::tags() const {
    return tags_;
}

std::vector<std::string> PlcSchemaStore::availableTagNames() const {
    std::vector<std::string> result;
    result.reserve(tags_.size());
    for (const auto& [name, _] : tags_)
        result.push_back(name);
    return result;
}

void PlcSchemaStore::clear() {
    dbs_.clear();
    dbs_by_name_.clear();
    udts_.clear();
    tags_.clear();
    warnings_.clear();
    base_dir_.clear();
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::saveToJsonFile(const std::string& t_path) const {
    std::ofstream file(t_path);
    if (!file.is_open())
        return Err::Generic("cannot open file for writing: {}", t_path);

    file << toJson(std::nullopt, false, true) << std::endl;
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::loadFile(
    const std::string& t_filepath, bool t_force, std::map<std::string, UdtDefinition>* tp_global_udts) {
    return SclCompiler::loadFile(*this, t_filepath, t_force, tp_global_udts);
}

sgrn::Result<void, ::sgrn::scl::Error> PlcSchemaStore::loadSchema(const std::string& t_path_or_content, bool t_force) {
    if (t_path_or_content.empty())
        return {};

    std::error_code ec;
    if (fs::exists(t_path_or_content, ec) && fs::is_regular_file(t_path_or_content, ec)) {
        return loadFile(t_path_or_content, t_force);
    }

    return SclCompiler::loadFromContent(*this, t_path_or_content, "embedded", t_force);
}

std::vector<uint16_t> PlcSchemaStore::availableDbs() const {
    std::vector<uint16_t> result;
    result.reserve(dbs_.size());
    for (const auto& [db_num, _] : dbs_)
        result.push_back(db_num);
    return result;
}

std::vector<std::string> PlcSchemaStore::availableDbNames() const {
    std::vector<std::string> result;
    result.reserve(dbs_by_name_.size());
    for (const auto& [name, _] : dbs_by_name_)
        result.push_back(name);
    return result;
}

std::string PlcSchemaStore::toJson(std::optional<uint16_t> t_db_number, bool t_headers_only, bool t_pretty) const {
    return SchemaSerializer::serialize(*this, t_db_number, t_headers_only, t_pretty);
}

} // namespace sgrn::scl
