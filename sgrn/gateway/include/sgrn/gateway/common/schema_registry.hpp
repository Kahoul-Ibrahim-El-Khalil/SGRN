#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace sgrn::gateway::common
{

/**
 * @brief Schema registry for iterating and processing database fields
 *
 * Callbacks use sgrn::Result pattern for error handling - no exceptions assumed.
 */
namespace schema_registry
{

using DbIterator = std::function<sgrn::Result<void, std::string>(uint16_t db_number, const sgrn::scl::DbSchema& t_schema)>;
using FieldIterator = std::function<sgrn::Result<void, std::string>(uint16_t db_number, const sgrn::scl::DbField& t_field)>;
using DbFieldPair = std::pair<uint16_t, const sgrn::scl::DbField*>;

/**
 * @brief Iterate over all databases in store and invoke callback
 * Stops at first error from callback
 */
inline sgrn::Result<void, std::string> forEachDb(const sgrn::scl::PlcSchemaStore& t_store, const DbIterator& t_callback) {

    for (const auto& [db_num, schema] : t_store.dbs()) {
        auto res = t_callback(db_num, schema);
        if (res.hasError())
            return res;
    }
    return {};
}

/**
 * @brief Iterate over all fields in all databases and invoke callback
 * Stops at first error from callback
 */
inline sgrn::Result<void, std::string> forEachField(const sgrn::scl::PlcSchemaStore& t_store, const FieldIterator& t_callback) {

    for (const auto& [db_num, schema] : t_store.dbs()) {
        for (const auto& field : schema.fields) {
            auto res = t_callback(db_num, field);
            if (res.hasError())
                return res;
        }
    }
    return {};
}

/**
 * @brief Get maximum field count across all databases
 */
inline uint32_t getMaxFieldCount(const sgrn::scl::PlcSchemaStore& t_store) {
    uint32_t max_fields = 0;
    for (const auto& [_, schema] : t_store.dbs()) {
        max_fields = std::max(max_fields, static_cast<uint32_t>(schema.fields.size()));
    }
    return std::max(1u, max_fields);
}

/**
 * @brief Get list of all database numbers
 */
inline std::vector<uint16_t> getAllDbs(const sgrn::scl::PlcSchemaStore& t_store) {
    std::vector<uint16_t> dbs;
    for (const auto& [db_num, _] : t_store.dbs()) {
        dbs.push_back(db_num);
    }
    return dbs;
}

/**
 * @brief Get all fields for a specific database
 */
inline sgrn::Result<std::vector<DbFieldPair>, std::string> getDbFields(const sgrn::scl::PlcSchemaStore& t_store, uint16_t t_db_number) {

    std::vector<DbFieldPair> fields;
    for (const auto& [db_num, schema] : t_store.dbs()) {
        if (db_num == t_db_number) {
            for (const auto& field : schema.fields) {
                fields.push_back({db_num, &field});
            }
            return fields;
        }
    }
    return "DB not found: " + std::to_string(t_db_number);
}

/**
 * @brief Count total fields across all databases
 */
inline size_t getTotalFieldCount(const sgrn::scl::PlcSchemaStore& t_store) {
    size_t total = 0;
    for (const auto& [_, schema] : t_store.dbs()) {
        total += schema.fields.size();
    }
    return total;
}

} // namespace schema_registry

} // namespace sgrn::gateway::common
