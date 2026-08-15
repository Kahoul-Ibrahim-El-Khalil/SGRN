#pragma once

#include <sgrn/gateway/adapters/http/path.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/strings.hpp>

namespace sgrn::gateway::common
{

/**
 * @brief Schema resolution utilities for protocol adapters
 */
namespace schema_resolver
{
/**
 * @brief Resolve a topic/path to schema information
 * @example "ReactorCore/speed" → {db=1, field="speed", array=nullopt}
 */
inline ::sgrn::gateway::adapters::Resolution resolve(std::string_view t_path, const sgrn::scl::PlcSchemaStore& t_store) {

    auto parts = sgrn::utils::strings::tokenize(t_path, '/');
    return adapters::resolveSemanticPath(parts, t_store);
}

/**
 * @brief Resolve a topic/path to just DB number
 * @return Result with DB number or error string
 */
inline sgrn::Result<uint16_t, std::string> resolveDb(std::string_view t_path, const sgrn::scl::PlcSchemaStore& t_store) {

    ::sgrn::gateway::adapters::Resolution res = resolve(t_path, t_store);
    if (res.schema) {
        return res.schema->db_number;
    }
    return "DB not found in path: " + std::string(t_path);
}

/**
 * @brief Resolve a topic/path to just field path
 * @return Result with field path or error string
 */
inline sgrn::Result<std::string, std::string> resolveField(std::string_view t_path, const sgrn::scl::PlcSchemaStore& t_store) {

    ::sgrn::gateway::adapters::Resolution res = resolve(t_path, t_store);
    if (!res.field_path.empty()) {
        return res.field_path;
    }
    return "Field path not found: " + std::string(t_path);
}
}; // namespace schema_resolver

} // namespace sgrn::gateway::common
