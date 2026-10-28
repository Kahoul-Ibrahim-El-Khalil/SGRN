#include <fmt/format.h>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/path.hpp>
#include <sgrn/gateway/twin/time_utils.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <algorithm>
#include <cstdint>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sgrn/scl/utils.hpp>

namespace sgrn::gateway::twin
{

using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;
using ::sgrn::scl::SclError;

// The JSON encode/decode layer is now owned by scl (sgrn/scl/utils.hpp) — a
// single implementation instead of a drifting twin fork. The twin namespace
// keeps its declared entry points as delegators so all in-repo call sites
// (twin, s7shell, plcsim tools) stay source-compatible; unified semantics:
// string tile capacity from string_capacity (count fallback), enum labels
// resolved in encodeScalarValue, string arrays encoded per-tile, DTL string
// parsing covers ISO/"T", space±ms and date-only forms.

sgrn::Result<void, ::sgrn::scl::SclError> encodeDtlValue(
    const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e) {
    return ::sgrn::scl::encodeDtlValue(t_value, tp_ptr, t_buffer_size, t_e);
}

sgrn::Result<void, ::sgrn::scl::SclError> encodeScalarValue(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e) {
    return ::sgrn::scl::encodeScalarValue(t_field, t_value, tp_ptr, t_buffer_size, t_e);
}

sgrn::Result<void, ::sgrn::scl::SclError> encodeArrayValue(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    return ::sgrn::scl::encodeArrayValue(t_field, t_value, tp_ptr, t_buffer_size, t_depth, t_e);
}

sgrn::Result<void, ::sgrn::scl::SclError> encodeFieldRapidJson(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    return ::sgrn::scl::encodeFieldRapidJson(t_field, t_value, tp_ptr, t_buffer_size, t_depth, t_e);
}

sgrn::Result<void, ::sgrn::scl::SclError> encodeFieldAt(
    const DbField& t_field, const std::string& t_value_json, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    return ::sgrn::scl::encodeFieldAt(t_field, t_value_json, tp_ptr, t_buffer_size, t_depth, t_e);
}

sgrn::Result<void, ::sgrn::scl::SclError> applyJsonPatchToFields(const std::vector<::sgrn::scl::DbField>& t_fields,
    const std::string& t_patch_json, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e) {
    return ::sgrn::scl::applyJsonPatchToFields(t_fields, t_patch_json, tp_ptr, t_buffer_size, t_e);
}

std::string parseSemanticValue(const std::string& t_raw) {
    return ::sgrn::scl::parseSemanticValue(t_raw);
}

std::string parseRawValuePayload(const std::string& t_raw) {
    return ::sgrn::scl::parseRawValuePayload(t_raw);
}

sgrn::Result<void, ::sgrn::scl::SclError> encodeValue(
    const DbField& t_field, const std::string& t_value, uint8_t* tp_buffer_ptr, size_t t_buffer_size) {
    return ::sgrn::gateway::twin::encodeFieldAt(t_field, t_value, tp_buffer_ptr, t_buffer_size);
}

namespace
{
// Byte size of one swappable unit, or 0 when the type must not be touched
// (1-byte units, bit-packed bools, strings, BCD-structured temporals).
size_t swapUnitSize(DataType t_type) {
    switch (t_type) {
        case DataType::Int:
        case DataType::UInt:
        case DataType::Word:
        case DataType::Date:
        case DataType::WChar:
        case DataType::Counter:
        case DataType::Timer:
            return 2;
        case DataType::DInt:
        case DataType::UDInt:
        case DataType::Real:
        case DataType::DWord:
        case DataType::Time:
        case DataType::TimeOfDay:
            return 4;
        case DataType::LInt:
        case DataType::ULInt:
        case DataType::LReal:
        case DataType::LWord:
        case DataType::LTime:
        case DataType::LTimeOfDay:
        case DataType::LDT:
        case DataType::LDTL:
            return 8;
        default:
            return 0;
    }
}

void swapIfCovered(uint8_t* tp_buf, size_t t_buf_base, size_t t_elem_off, size_t t_unit, size_t t_range_base, size_t t_range_end) {
    if (t_unit <= 1 || tp_buf == nullptr)
        return;
    if (t_elem_off >= t_range_base && t_elem_off + t_unit <= t_range_end && t_elem_off >= t_buf_base)
        std::reverse(tp_buf + (t_elem_off - t_buf_base), tp_buf + (t_elem_off - t_buf_base) + t_unit);
}

void swapFieldRange(const DbField& t_field, size_t t_abs_off, uint8_t* tp_buf, size_t t_buf_base, size_t t_range_base, size_t t_range_end) {
    if (t_field.endianness != s7codec::Endian::Little)
        return; // big-endian fields already match S7 wire order
    if (t_field.type == DataType::Struct) {
        const size_t elem_span = t_field.struct_size > 0 ? t_field.struct_size : 0;
        const uint32_t count = t_field.count > 1 ? t_field.count : 1;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t base = t_abs_off + static_cast<size_t>(i) * elem_span;
            for (const auto& child : t_field.children)
                swapFieldRange(child, base + static_cast<size_t>(child.offset), tp_buf, t_buf_base, t_range_base, t_range_end);
        }
        return;
    }
    if (t_field.type == DataType::DTL) {
        // 12-byte layout: year U16 @0, six single bytes @2..7, nanosecond
        // U32 @8. Only year and nanosecond are endian-sensitive.
        const uint32_t count = t_field.count > 1 ? t_field.count : 1;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t base = t_abs_off + static_cast<size_t>(i) * 12;
            swapIfCovered(tp_buf, t_buf_base, base, 2, t_range_base, t_range_end);
            swapIfCovered(tp_buf, t_buf_base, base + 8, 4, t_range_base, t_range_end);
        }
        return;
    }
    // NOTE: DateTime (DT) is fixed BCD byte order on both encode and decode
    // regardless of endianness — nothing to swap.
    const size_t unit = swapUnitSize(t_field.type);
    if (unit == 0)
        return;
    if (t_field.count > 1 && !t_field.is_dynamic) {
        for (uint32_t i = 0; i < t_field.count; ++i)
            swapIfCovered(tp_buf, t_buf_base, t_abs_off + static_cast<size_t>(i) * unit, unit, t_range_base, t_range_end);
        return;
    }
    if (t_field.count > 1 && t_field.is_dynamic) {
        // Dynamic array: U32 element count header (in field endianness)
        // followed by the elements.
        if (t_abs_off + 4 > t_range_end || t_abs_off < t_range_base || t_abs_off < t_buf_base)
            return;
        const uint32_t n = s7codec::fromEndian<uint32_t>(tp_buf + (t_abs_off - t_buf_base), t_field.endianness);
        const uint32_t cap = t_field.count;
        swapIfCovered(tp_buf, t_buf_base, t_abs_off, 4, t_range_base, t_range_end);
        for (uint32_t i = 0; i < std::min(n, cap); ++i)
            swapIfCovered(tp_buf, t_buf_base, t_abs_off + 4 + static_cast<size_t>(i) * unit, unit, t_range_base, t_range_end);
        return;
    }
    swapIfCovered(tp_buf, t_buf_base, t_abs_off, unit, t_range_base, t_range_end);
}
} // namespace

void swapFieldToBigEndian(const DbField& t_field, uint8_t* tp_buf, size_t t_size) {
    if (tp_buf == nullptr || t_size == 0 || t_field.endianness != s7codec::Endian::Little)
        return;
    DbField root = t_field;
    root.offset = 0;
    swapFieldRange(root, 0, tp_buf, 0, 0, t_size);
}

void swapRangeToBigEndian(
    const std::vector<::sgrn::scl::DbField>& t_fields, uint8_t* tp_buf, size_t t_buf_base_offset, size_t t_base_offset, size_t t_len) {
    if (tp_buf == nullptr || t_len == 0)
        return;
    const size_t range_end = t_base_offset + t_len;
    for (const auto& field : t_fields) {
        if (field.endianness != s7codec::Endian::Little)
            continue;
        const size_t span = static_cast<size_t>(::sgrn::gateway::twin::fieldSpanSize(field));
        const auto abs_off = static_cast<size_t>(field.offset);
        if (abs_off >= range_end || abs_off + span <= t_base_offset)
            continue; // no overlap
        swapFieldRange(field, abs_off, tp_buf, t_buf_base_offset, t_base_offset, range_end);
    }
}

} // namespace sgrn::gateway::twin
