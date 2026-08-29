#pragma once

#include <sgrn/SchemaStore.hpp>
#include <sgrn/scl/types.hpp>

#include <deque>
#include <map>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::scl
{
/**
 * @brief Result of a field location lookup.
 */
struct FieldLocation {
    uint16_t db_number{0};
    std::string db_name;
    const DbField* field{nullptr};
    int abs_offset{0}; // byte offset within the DB's raw buffer
};

/**
 * @brief Result of parseFieldTarget: split "Configuration.SetPoint" into DB + field.
 */
struct FieldTarget {
    uint16_t db_number{0};
    std::string field_path;
};

/**
 * @brief DB-centric schema directory manager.
 */
class PlcSchemaStore : public ::sgrn::SchemaStore {
    friend class SclCompiler;
    friend class SchemaSerializer;

public:
    // ── Factory methods (loading) ─────────────────────────────────────────────
    static sgrn::Result<PlcSchemaStore, SclError> loadFromDirectory(const std::string& t_symbols_dir, bool t_force = false);
    static sgrn::Result<PlcSchemaStore, SclError> loadFromFile(const std::string& t_filepath, bool t_force = false);
    static sgrn::Result<PlcSchemaStore, SclError> loadFromFiles(const std::vector<std::string>& t_filepaths, bool t_force = false);
    static sgrn::Result<PlcSchemaStore, SclError> loadFromJsonFile(const std::string& t_path);
    static sgrn::Result<PlcSchemaStore, SclError> loadFromJson(const rapidjson::Value& t_root);

    // ── DB schema queries ─────────────────────────────────────────────────────

    bool hasDb(uint16_t t_db_number) const;
    bool hasDb(const std::string& t_db_name) const;

    sgrn::Result<const DbSchema*, SclError> getDb(uint16_t t_db_number) const;
    sgrn::Result<const DbSchema*, SclError> getDbByName(const std::string& t_db_name) const;

    // ── Field lookup ─────────────────────────────────────────────────────────

    std::optional<FieldLocation> findField(uint16_t t_db_number, std::string_view t_field_path) const;

    std::optional<FieldLocation> findField(std::string_view t_db_name, std::string_view t_field_path) const;

    // ── DB reference resolution ──────────────────────────────────────────────

    std::optional<uint16_t> resolveDbRef(std::string_view t_token) const;
    std::optional<FieldTarget> parseFieldTarget(std::string_view t_path) const;

    // ── UDT queries ───────────────────────────────────────────────────────────

    const std::deque<UdtDefinition>& udts() const;
    bool hasUdt(uint16_t t_udt_number) const;
    bool hasUdt(const std::string& t_udt_name) const;
    sgrn::Result<const UdtDefinition*, SclError> getUdt(uint16_t t_udt_number) const;
    sgrn::Result<const UdtDefinition*, SclError> getUdtByName(const std::string& t_udt_name) const;

    // ── Mutation (add/remove) ─────────────────────────────────────────────────

    sgrn::Result<void, SclError> addDb(DbSchema&& t_reg, bool t_force = false, bool t_rebuild_index = true);
    sgrn::Result<void, SclError> removeDb(uint16_t t_db_number);

    sgrn::Result<void, SclError> addUdt(UdtDefinition&& t_udt, bool t_force = false, bool t_rebuild_index = true);
    sgrn::Result<void, SclError> removeUdt(uint16_t t_udt_number);

    sgrn::Result<void, SclError> addTag(PlcTag&& t_tag, bool t_force = false);
    sgrn::Result<void, SclError> removeTag(const std::string& t_tag_name);
    sgrn::Result<const PlcTag*, SclError> getTag(const std::string& t_tag_name) const;

    const std::map<std::string, PlcTag>& tags() const;

    // ── Enumeration ───────────────────────────────────────────────────────────

    std::vector<uint16_t> availableDbs() const;
    std::vector<std::string> availableDbNames() const;
    std::vector<std::string> availableTagNames() const;

    // ── Persistence ───────────────────────────────────────────────────────────

    void clear();
    sgrn::Result<void, SclError> saveToJsonFile(const std::string& t_path) const;
    sgrn::Result<void, SclError> loadFile(
        const std::string& t_filepath, bool t_force = false, std::map<std::string, UdtDefinition>* tp_global_udts = nullptr);

    sgrn::Result<void, SclError> loadSchema(const std::string& t_path_or_content, bool t_force = false);

    std::string toJson(std::optional<uint16_t> t_db_number = std::nullopt, bool t_headers_only = false, bool t_pretty = false) const;

    // ── Diagnostics ───────────────────────────────────────────────────────────

    const std::vector<std::string>& warnings() const {
        return warnings_;
    }
    void addWarning(std::string t_warning) {
        warnings_.push_back(std::move(t_warning));
    }
    const std::string& getBaseDir() const {
        return base_dir_;
    }

    const std::map<uint16_t, DbSchema>& dbs() const {
        return dbs_;
    }

private:
    std::string base_dir_;
    std::map<uint16_t, DbSchema> dbs_;
    std::map<std::string, const DbSchema*> dbs_by_name_;

    std::deque<UdtDefinition> udts_;
    std::map<uint16_t, const UdtDefinition*> udts_by_number_;
    std::map<std::string, const UdtDefinition*> udts_by_name_;

    std::map<std::string, PlcTag> tags_;
    std::vector<std::string> warnings_;

    void rebuildIndices();
};

} // namespace sgrn::scl
