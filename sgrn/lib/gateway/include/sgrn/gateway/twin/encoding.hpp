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
// Byte-swap multi-byte scalar elements covered by the DB-absolute range
// [base_offset, base_offset+len) between block-endian arena layout and S7-wire
// big-endian. tp_buf[0] corresponds to DB offset buf_base_offset (use the
// same value as base_offset for full-DB buffers, or the span start for
// request windows). Symmetric: the same call converts back. Only
// fully-covered elements are swapped; partial edge elements are left
// untouched. Covers multi-byte numerics, floats, LDT/LDTL and DTL (year U16
// + nanosecond U32). Skips 1-byte units, bit-packed bools, strings and
// DateTime (fixed BCD byte order, endian-invariant). Big-endian fields are
// left as-is, so big-endian blocks cost only the walk.
void swapRangeToBigEndian(
    const std::vector<::sgrn::scl::DbField>& t_fields, uint8_t* tp_buf, size_t t_buf_base_offset, size_t t_base_offset, size_t t_len);
// Whole-buffer variant for field-aligned wire payloads (DbIOProvider
// put/commit): tp_buf holds exactly the field's span. No-op unless the
// field is little-endian.
void swapFieldToBigEndian(const ::sgrn::scl::DbField& t_field, uint8_t* tp_buf, size_t t_size);
} // namespace sgrn::gateway::twin
