#pragma once

#include <fmt/core.h>
#include <sgrn/scl/types.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/s7.hpp>

namespace sgrn::gateway::adapters::s7
{
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;

constexpr int kMaxJsonRecursion = 10;

/**
 * @brief Checks if a memory range [offset, offset + size) overlaps with any of the dirty ranges.
 */
inline bool overlapsAnyRange(int t_offset, int t_size, const std::vector<std::pair<int32_t, int32_t>>& t_ranges) {
    if (t_ranges.empty())
        return true; // No filter means everything is dirty (baseline)
    for (const auto& r : t_ranges) {
        if (t_offset < r.second && (t_offset + t_size) > r.first)
            return true;
    }
    return false;
}

/**
 * @brief Directly serializes an S7 field value from a memory buffer to a RapidJSON Writer.
 *
 * This avoids intermediate Json::Value (JsonCpp) objects and heap allocations.
 */
template <typename Writer>
void serializeFieldTo(Writer& t_writer, const DbField& t_field, const uint8_t* p_ptr, size_t t_buffer_size) {
    auto dv = s7codec::decodeScalar(t_field.type, p_ptr, t_buffer_size, t_field.bit_index, t_field.count);

    if (!dv.valid()) {
        t_writer.Null();
        return;
    }
    switch (dv.kind()) {
        case s7codec::ValueKind::Bool:
            t_writer.Bool(dv.b());
            break;
        case s7codec::ValueKind::SignedInt:
            if (t_field.type == DataType::Time) {
                std::string s = s7codec::formatTimeString(static_cast<int32_t>(dv.i()));
                t_writer.String(s.c_str(), static_cast<rapidjson::SizeType>(s.length()));
            } else {
                t_writer.Int64(dv.i());
            }
            break;
        case s7codec::ValueKind::UnsignedInt:
            if (t_field.type == DataType::Byte) {
                std::string s = fmt::format("0x{:02X}", static_cast<uint8_t>(dv.u()));
                t_writer.String(s.c_str(), static_cast<rapidjson::SizeType>(s.length()));
            } else if (t_field.type == DataType::Word) {
                std::string s = fmt::format("0x{:04X}", static_cast<uint16_t>(dv.u()));
                t_writer.String(s.c_str(), static_cast<rapidjson::SizeType>(s.length()));
            } else if (t_field.type == DataType::DWord) {
                std::string s = fmt::format("0x{:08X}", static_cast<uint32_t>(dv.u()));
                t_writer.String(s.c_str(), static_cast<rapidjson::SizeType>(s.length()));
            } else {
                t_writer.Uint64(dv.u());
            }
            break;
        case s7codec::ValueKind::Float:
            t_writer.Double(static_cast<double>(dv.f()));
            break;
        case s7codec::ValueKind::Double:
            t_writer.Double(dv.d());
            break;
        case s7codec::ValueKind::String:
            t_writer.String(dv.s().c_str(), static_cast<rapidjson::SizeType>(dv.s().length()));
            break;
        default:
            t_writer.Null();
            break;
    }
}

/**
 * @brief Recursive serialization for structs and arrays using RapidJSON Writer.
 * Includes safety bounds and recursion depth limits.
 */
template <typename Writer>
void serializeComplexFieldTo(Writer& t_writer, const DbField& t_field, const uint8_t* tp_base_ptr, size_t t_buffer_size,
    const std::vector<std::pair<int32_t, int32_t>>& t_ranges = {}, int t_depth = 0) {
    if (t_depth > kMaxJsonRecursion) {
        t_writer.String("!!! MAX_DEPTH_EXCEEDED !!!");
        return;
    }

    // Safety: ensure field offset is within buffer
    if (t_field.offset < 0 || static_cast<size_t>(t_field.offset) >= t_buffer_size) {
        t_writer.Null();
        return;
    }

    const uint8_t* p_ptr = tp_base_ptr + t_field.offset;

    if (t_field.type == DataType::Struct) {
        if (t_field.count > 1) {
            t_writer.StartArray();
            int struct_size = std::max(1, t_field.struct_size);
            for (int i = 0; i < t_field.count; ++i) {
                int elem_offset = t_field.offset + (i * struct_size);
                if (static_cast<size_t>(elem_offset + struct_size) > t_buffer_size)
                    break;

                // Check if this specific array element is dirty
                if (!overlapsAnyRange(elem_offset, struct_size, t_ranges))
                    continue;

                t_writer.StartObject();
                for (const auto& child : t_field.children) {
                    if (overlapsAnyRange(elem_offset + child.offset, s7codec::typeSpanBytes(child.type, child.count), t_ranges)) {
                        t_writer.Key(child.name.c_str(), static_cast<rapidjson::SizeType>(child.name.length()));
                        serializeComplexFieldTo(
                            t_writer, child, tp_base_ptr + elem_offset, t_buffer_size - elem_offset, t_ranges, t_depth + 1);
                    }
                }
                t_writer.EndObject();
            }
            t_writer.EndArray();
        } else {
            t_writer.StartObject();
            for (const auto& child : t_field.children) {
                // Nested check: only serialize dirty children
                if (overlapsAnyRange(t_field.offset + child.offset, s7codec::typeSpanBytes(child.type, child.count), t_ranges)) {
                    t_writer.Key(child.name.c_str(), static_cast<rapidjson::SizeType>(child.name.length()));
                    serializeComplexFieldTo(t_writer, child, p_ptr, t_buffer_size - t_field.offset, t_ranges, t_depth + 1);
                }
            }
            t_writer.EndObject();
        }
    } else if (t_field.count > 1 && t_field.type != DataType::String && t_field.type != DataType::WString) {
        t_writer.StartArray();
        int elem_size = s7codec::primitiveSize(t_field.type);
        for (int i = 0; i < t_field.count; ++i) {
            int elem_offset = t_field.offset + (i * elem_size);
            if (static_cast<size_t>(elem_offset + elem_size) > t_buffer_size)
                break;

            if (overlapsAnyRange(elem_offset, elem_size, t_ranges)) {
                serializeFieldTo(t_writer, t_field, tp_base_ptr + elem_offset, t_buffer_size - elem_offset);
            }
        }
        t_writer.EndArray();
    } else {
        serializeFieldTo(t_writer, t_field, p_ptr, t_buffer_size - static_cast<size_t>(t_field.offset));
    }
}

} // namespace sgrn::gateway::adapters::s7
