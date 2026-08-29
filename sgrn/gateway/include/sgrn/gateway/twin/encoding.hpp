#pragma once
#include <sgrn/Result.hpp>
#include <sgrn/scl/types.hpp>
#include <rapidjson/document.h>
#include <s7codec/codec.hpp>
#include <string>
#include <vector>

namespace sgrn::gateway::twin
{
sgrn::Result<void, ::sgrn::scl::SclError> encodeFieldAt(const ::sgrn::scl::DbField& t_field, const std::string& t_value_json,
    uint8_t* tp_ptr, size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);
sgrn::Result<void, ::sgrn::scl::SclError> encodeFieldRapidJson(const ::sgrn::scl::DbField& t_field, const rapidjson::Value& t_value,
    uint8_t* tp_ptr, size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);
sgrn::Result<void, ::sgrn::scl::SclError> encodeScalarValue(const ::sgrn::scl::DbField& t_field, const rapidjson::Value& t_value,
    uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e = s7codec::Endian::Big);
sgrn::Result<void, ::sgrn::scl::SclError> encodeArrayValue(const ::sgrn::scl::DbField& t_field, const rapidjson::Value& t_value,
    uint8_t* tp_ptr, size_t t_buffer_size, int t_depth = 0, s7codec::Endian t_e = s7codec::Endian::Big);
sgrn::Result<void, ::sgrn::scl::SclError> encodeDtlValue(
    const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e = s7codec::Endian::Big);
sgrn::Result<void, ::sgrn::scl::SclError> encodeValue(
    const ::sgrn::scl::DbField& t_field, const std::string& t_value, uint8_t* tp_buffer_ptr, size_t t_buffer_size);
sgrn::Result<void, ::sgrn::scl::SclError> applyJsonPatchToFields(const std::vector<::sgrn::scl::DbField>& t_fields,
    const std::string& t_patch_json, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e = s7codec::Endian::Big);
std::string parseSemanticValue(const std::string& t_raw);
std::string parseRawValuePayload(const std::string& t_raw);
} // namespace sgrn::gateway::twin
