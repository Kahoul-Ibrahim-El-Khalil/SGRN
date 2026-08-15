#pragma once

#include <sgrn/scl/types.hpp>
#include <rapidjson/document.h>

#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/utils.hpp>

namespace sgrn::scl
{

/**
 * @brief Serialization engine for S7 Semantic Registries.
 *
 * Handles conversion between internal structures and JSON.
 */
class SchemaSerializer {
public:
    static std::string udtToJson(const UdtDefinition& t_udt);
    static std::string dbToJson(const DataBlockRegistry& t_db);
    static std::string tagToJson(const PlcTag& t_tag);

    static sgrn::Result<UdtDefinition, ::sgrn::scl::Error> udtFromJson(const rapidjson::Value& t_node);
    static sgrn::Result<DataBlockRegistry, ::sgrn::scl::Error> dbFromJson(const rapidjson::Value& t_node);
    static sgrn::Result<PlcTag, ::sgrn::scl::Error> tagFromJson(const rapidjson::Value& t_node);

    /**
     * @brief Serializes the registry to JSON string, with optional filtering.
     * Uses rapidjson for efficient serialization.
     */
    static std::string serialize(const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number = std::nullopt,
        bool t_headers_only = false, bool t_pretty = false);

    /**
     * @brief Deserializes a JSON object into a registry.
     */
    static sgrn::Result<void, ::sgrn::scl::Error> deserialize(PlcSchemaStore& t_registry, const rapidjson::Value& t_root);

private:
    static void resolveUdtsInRegistry(PlcSchemaStore& t_registry);
};

} // namespace sgrn::scl
