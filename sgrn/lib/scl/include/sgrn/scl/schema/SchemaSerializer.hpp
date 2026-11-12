#pragma once

#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/scl/utils.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
 * @brief Domain content of a parsed schema JSON document.
 *
 * Filled by SchemaSerializer::parseContent from either the object form
 * ({dbs, udts, tags, dimensions, warnings}) or the legacy bare array of DBs.
 */
struct RegistryContent {
    std::vector<DbSchema> dbs;
    std::vector<UdtDefinition> udts;
    std::vector<PlcTag> tags;
    std::vector<std::string> dimensions;
    std::vector<std::string> warnings;
};

/**
 * @brief Serialization engine for S7 Semantic Registries.
 *
 * JSON goes through the Glaze-based sgrn::json surface: wire structs (private
 * to the .cpp) carry the historical on-disk layout, and reflection does the
 * rest. The binary encoding below is the lossless form used by WAL archives.
 */
class SchemaSerializer {
public:
    // ── JSON write ─────────────────────────────────────────────────────────────
    static std::string udtToJson(const UdtDefinition& t_udt);
    static std::string dbToJson(const DbSchema& t_db);
    static std::string tagToJson(const PlcTag& t_tag);

    /**
     * @brief Serializes the registry to a JSON string, with optional filtering.
     *
     * @param t_db_number  Only emit this DB's header+fields when set.
     * @param t_headers_only  Omit the "fields" arrays (DB headers only).
     * @param t_pretty  Pretty-print (human-facing output).
     */
    static std::string serialize(const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number = std::nullopt,
        bool t_headers_only = false, bool t_pretty = false);

    // ── JSON read (text in, domain out) ────────────────────────────────────────
    static sgrn::Result<UdtDefinition, scl::SclError> udtFromJson(std::string_view t_text);
    static sgrn::Result<DbSchema, scl::SclError> dbFromJson(std::string_view t_text);
    static sgrn::Result<PlcTag, scl::SclError> tagFromJson(std::string_view t_text);

    /**
     * @brief Parses schema text into RegistryContent.
     *
     * Accepts the object form and the legacy bare array of DBs. With
     * t_lenient_tags, malformed tag rows are skipped instead of failing the
     * whole parse (per-entity JSON consumers).
     */
    static sgrn::Result<RegistryContent, scl::SclError> parseContent(std::string_view t_text, bool t_lenient_tags = false);

    /**
     * @brief Deserializes schema JSON text into a registry.
     *
     * Loads dbs/udts/tags, then rebuilds indices and resolves UDT references
     * so a JSON-loaded store compares equal to a binary-loaded one.
     */
    static sgrn::Result<void, scl::SclError> deserialize(PlcSchemaStore& t_registry, std::string_view t_text);

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
