#pragma once
#include <fmt/format.h>
#include <sgrn/scl/types.hpp>
#include <sgrn/utils/strings.hpp>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::scl
{

// -----------------------------------------------------------------------------
// DB Field Utilities
// -----------------------------------------------------------------------------

struct LocatedField {
    const DbField* field{nullptr};
    int abs_offset{0};
    int bit_index{-1}; // -1 = "use field->bit_index"; >=0 = resolved element bit for array access
};

/**
 * @brief Recursively find a field by its dot-separated path (e.g. "Struct.SubField").
 */
std::optional<LocatedField> findFieldByPath(const std::vector<DbField>& t_fields, const std::string& t_path, int t_base_offset = 0);

/**
 * @brief Recursively apply a JSON patch object to a set of schema fields.
 */
sgrn::Result<void, ::sgrn::scl::Error> applyJsonPatchToFields(const std::vector<DbField>& t_fields, const std::string& t_patch_json,
    uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e = s7codec::Endian::Big);

// -----------------------------------------------------------------------------
// Time Utilities
// -----------------------------------------------------------------------------

/**
 * @brief Parses an S7 Time string in format "#T19:10:10:100" or "[-]HH:MM:SS.mmm".
 * Returns total milliseconds.
 */
std::optional<int64_t> parseS7Time(const std::string& t_raw);

// -----------------------------------------------------------------------------
// JSON Utilities
// -----------------------------------------------------------------------------

/**
 * @brief Attempts to parse a string into a typed JSON string (bool, hex, double, int, or string).
 */
std::string parseSemanticValue(const std::string& t_raw);

/**
 * @brief Parses a raw string input into a JSON string payload, supporting full JSON, CSV arrays, or semantic values.
 */
std::string parseRawValuePayload(const std::string& t_raw);

sgrn::Result<void, ::sgrn::scl::Error> encodeDtlValue(
    const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e = s7codec::Endian::Big);

int fieldSpanSize(const DbField& t_field);

sgrn::Result<void, ::sgrn::scl::Error> encodeScalarValue(const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr,
    size_t t_buffer_size, s7codec::Endian t_e = s7codec::Endian::Big);

sgrn::Result<std::string, ::sgrn::scl::Error> decodeFieldAt(
    const DbField& t_field, const uint8_t* tp_ptr, size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);

sgrn::Result<void, ::sgrn::scl::Error> encodeFieldRapidJson(const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr,
    size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);

sgrn::Result<void, ::sgrn::scl::Error> encodeFieldAt(const DbField& t_field, const std::string& t_value_json, uint8_t* tp_ptr,
    size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);

/**
 * @brief Decodes a single DB buffer into a JSON string using the provided registry.
 */
std::string decodeDbBuffer(const sgrn::scl::DataBlockRegistry& t_reg, const uint8_t* tp_buf, size_t t_buffer_size);

int symbolFieldSpanBytes(const DbField& t_field);
const DbField* findFieldByName(const DataBlockRegistry& t_reg, const std::string& t_name);

sgrn::Result<std::vector<uint8_t>, Error> parseHexBytes(const std::string& t_joined);
std::optional<PlcAddress> parsePlcAddress(const std::string& t_tok);
std::optional<uint64_t> parseCLiteral(const std::string& t_raw);
bool hasCLiteralPrefix(const std::string& t_val);
std::vector<uint8_t> uint64ToBytesBE(uint64_t t_val, int t_n);
std::string byteToBinStr(uint8_t t_b);
std::optional<PlcDbRawAddr> parsePlcDbRawAddr(const std::string& t_target);
std::optional<uint16_t> parseDbRef(const std::string& t_val);
std::optional<uint16_t> resolveDbRef(const std::string& t_token, const std::map<std::string, uint16_t>& t_name_to_number);
std::optional<std::pair<uint16_t, std::string>> parseFieldTarget(
    const std::string& t_target, const std::map<std::string, uint16_t>& t_name_to_number);
std::string_view plcAreaName(int t_area);

} // namespace sgrn::scl
