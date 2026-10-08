#pragma once

#include <sgrn/scl/types.hpp>
#include <rapidjson/document.h>

#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/utils.hpp>

#include <string_view>

namespace sgrn::scl
{

/**
 * @brief Magic prefix of a binary-encoded schema payload ("SGRS").
 *
 * Lets readers distinguish the binary encoding from the legacy JSON schema
 * text (which starts with '{') without consulting the archive version.
 */
inline constexpr char kBinarySchemaMagic[4] = {'S', 'G', 'R', 'S'};

/// Binary schema codec version stamped after the magic. Bump when the
/// layout below changes; decoders reject anything else.
// v2 adds PlcTag.udt_name (TIA-style UDT-typed tag rows from #TAG_TABLE).
inline constexpr uint16_t kBinarySchemaCodecVersion = 2;

/// True when t_bytes holds a binary-encoded schema (magic sniffing).
inline bool isBinarySchemaPayload(std::string_view t_bytes) {
    return t_bytes.size() >= 4 && t_bytes[0] == kBinarySchemaMagic[0] && t_bytes[1] == kBinarySchemaMagic[1] &&
           t_bytes[2] == kBinarySchemaMagic[2] && t_bytes[3] == kBinarySchemaMagic[3];
}

/**
 * @brief Serialization engine for S7 Semantic Registries.
 *
 * Handles conversion between internal structures and JSON.
 */
class SchemaSerializer {
public:
    static std::string udtToJson(const UdtDefinition& t_udt);
    static std::string dbToJson(const DbSchema& t_db);
    static std::string tagToJson(const PlcTag& t_tag);

    static sgrn::Result<UdtDefinition, scl::SclError> udtFromJson(const rapidjson::Value& t_node);
    static sgrn::Result<DbSchema, scl::SclError> dbFromJson(const rapidjson::Value& t_node);
    static sgrn::Result<PlcTag, scl::SclError> tagFromJson(const rapidjson::Value& t_node);

    /**
     * @brief Serializes the registry to JSON string, with optional filtering.
     * Uses rapidjson for efficient serialization.
     */
    static std::string serialize(const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number = std::nullopt,
        bool t_headers_only = false, bool t_pretty = false);

    /**
     * @brief Deserializes a JSON object into a registry.
     */
    static sgrn::Result<void, scl::SclError> deserialize(PlcSchemaStore& t_registry, const rapidjson::Value& t_root);

    /**
     * @brief Serializes the registry to the binary schema encoding.
     *
     * Compact, lossless (unlike the JSON form it keeps array bounds, init
     * values, UDT alias detail, and full tag addresses), little-endian,
     * bounds-checked on decode. Used for the binary WAL archive header
     * (archive version 4+); the JSON form stays for files and JSONL WALs.
     */
    static sgrn::Result<std::string, scl::SclError> serializeBinary(const PlcSchemaStore& t_registry);

    /**
     * @brief Deserializes a binary schema payload into a registry.
     *
     * Mirrors deserialize()'s tail (index rebuild + UDT resolution) so a
     * binary-loaded store compares equal to a JSON-loaded one. Fails closed
     * (ParseError/UnsupportedType/InvalidType) on bad magic, unknown codec
     * version, truncation, or unknown type names.
     */
    static sgrn::Result<void, scl::SclError> deserializeBinary(PlcSchemaStore& t_registry, std::string_view t_bytes);

private:
    static void resolveUdtsInRegistry(PlcSchemaStore& t_registry);
};

} // namespace sgrn::scl
