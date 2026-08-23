#pragma once

#include <sgrn/gateway/adapters/opcua/encoders.hpp>

namespace sgrn::gateway::adapters
{
// tryEncodePrimitive removed — dispatch is now table-driven via sgrn::codecs::kCodecTable.
using s7codec::encodeString;
using s7codec::encodeWString;
using s7codec::encodeXString;
using s7codec::encodeXWString;
using scl::DataType;
using sgrn::gateway::twin::PlcNode;

Result<void, OpcUaAdapterError> encodeStringOpcUaToMemory(const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const PlcNode& t_node) {

    const auto* p_string = reinterpret_cast<const UA_String*>(tp_ua_ptr);
    auto p_string_data_char = reinterpret_cast<const char*>(p_string->data);
    const int string_length = static_cast<int>(p_string->length);
    const uint32_t string_capacity = t_node.string_capacity_;
    if (t_node.type_ == DataType::String) {
        if (auto result = encodeString(p_string_data_char, string_length, string_capacity, tp_memory, t_node.size_); !result.has_value()) {
            return Error(OpcUaAdapterError::OUT_OF_RANGE);
        }

    }

    else if (t_node.type_ == DataType::XString) {
        if (auto result = encodeXString(p_string_data_char, string_length, string_capacity, tp_memory, t_node.size_, t_node.endian_);
            !result.has_value()) {
            return Error(OpcUaAdapterError::OUT_OF_RANGE);
        }

    }

    else if (t_node.type_ == DataType::WString || t_node.type_ == DataType::XWString) {
        std::string utf8(p_string_data_char, string_length);
        auto wide = sgrn::utils::strings::utf8ToUtf16(utf8);
        const uint16_t* p_string_wide_char = reinterpret_cast<const uint16_t*>(wide->c_str());
        const int wide_string_length = wide->size();
        if (wide) {
            if (t_node.type_ == DataType::WString) {
                if (auto result =
                        encodeWString(p_string_wide_char, wide_string_length, string_capacity, tp_memory, t_node.size_, t_node.endian_);
                    !result.has_value()) {

                    return Error(OpcUaAdapterError::OUT_OF_RANGE);
                }
            } else {
                if (auto result =
                        encodeXWString(p_string_wide_char, wide_string_length, string_capacity, tp_memory, t_node.size_, t_node.endian_);
                    !result.has_value()) {
                    return Error(OpcUaAdapterError::OUT_OF_RANGE);
                }
            }
        }
    }
    return {};
}

Result<void, OpcUaAdapterError> encodeDateTimeOpcUaToMemory(
    const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const PlcNode& t_node, size_t s7_size) {
    const UA_DateTime ua_date = *reinterpret_cast<const UA_DateTime*>(tp_ua_ptr);
    // UA_DateTime_toUnixTime() returns Unix time in SECONDS.
    const int64_t unix_time_sec = UA_DateTime_toUnixTime(ua_date);
    if (t_node.type_ == DataType::DTL) {
        if (unix_time_sec < 0 || unix_time_sec > 9223372036LL)
            return Error(OpcUaAdapterError::OUT_OF_RANGE);
        struct tm tm_info = resolveOpcUaLocalTime(ua_date);
        UA_DateTimeStruct dts = UA_DateTime_toStruct(ua_date);
        s7codec::DtlComponents dtl;
        dtl.year = tm_info.tm_year + 1900;
        dtl.month = tm_info.tm_mon + 1;
        dtl.day = tm_info.tm_mday;
        dtl.day_of_week = tm_info.tm_wday + 1;
        dtl.hour = tm_info.tm_hour;
        dtl.minute = tm_info.tm_min;
        dtl.second = tm_info.tm_sec;
        dtl.nanosecond = (dts.milliSec * 1000000) + (dts.microSec * 1000) + dts.nanoSec;
        if (auto result = s7codec::encodeDtl(dtl, tp_memory, s7_size, t_node.endian_); !result.has_value())
            return Error(OpcUaAdapterError::ENCODE_FAILED);
    } else if (t_node.type_ == DataType::DateTime) {
        if (unix_time_sec < 631152000LL)
            return Error(OpcUaAdapterError::OUT_OF_RANGE);
        struct tm tm_info = resolveOpcUaLocalTime(ua_date);
        if (auto result = s7codec::encodeDateTime(tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour,
                tm_info.tm_min, tm_info.tm_sec, 0, tp_memory, s7_size);
            !result.has_value())
            return Error(OpcUaAdapterError::ENCODE_FAILED);
    }
    return {};
}

Result<void, OpcUaAdapterError> encodeScalarOpcUaToMemory(
    const UA_DataType* tp_ua_type, const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const PlcNode& t_node) {
    const size_t s7_size = s7codec::primitiveSize(t_node.type_);

    if (tp_ua_type && tp_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {
        const UA_Int32 raw = *reinterpret_cast<const UA_Int32*>(tp_ua_ptr);
        s7codec::DecodedValue dv = s7codec::DecodedValue::makeSigned(static_cast<int64_t>(raw));
        auto sc = s7codec::encodeScalar(dv, t_node.type_, tp_memory, s7_size, t_node.bit_index_, 0, t_node.endian_);
        if (!sc.has_value())
            return Error(OpcUaAdapterError::ENCODE_FAILED);
        return {};
    }

    if (tp_ua_type == &UA_TYPES[UA_TYPES_STRING]) {
        return encodeStringOpcUaToMemory(tp_ua_ptr, tp_memory, t_node);
    }
    if (tp_ua_type == &UA_TYPES[UA_TYPES_DATETIME]) {
        return encodeDateTimeOpcUaToMemory(tp_ua_ptr, tp_memory, t_node, s7_size);
    }

    // ── Engineering-range guard ──────────────────────────────────────────────
    // Covers scalar leaves, array elements, AND struct members alike, since
    // this function is the single choke point all three funnel through
    // (see encodeStructOpcUaToMemory's per-member and per-array-element calls).
    if (t_node.min_val_.has_value() || t_node.max_val_.has_value()) {
        std::optional<double> v;
        if (tp_ua_type == &UA_TYPES[UA_TYPES_DOUBLE])
            v = *reinterpret_cast<const UA_Double*>(tp_ua_ptr);
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_FLOAT])
            v = static_cast<double>(*reinterpret_cast<const UA_Float*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_INT64])
            v = static_cast<double>(*reinterpret_cast<const UA_Int64*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_UINT64])
            v = static_cast<double>(*reinterpret_cast<const UA_UInt64*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_INT32])
            v = static_cast<double>(*reinterpret_cast<const UA_Int32*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_UINT32])
            v = static_cast<double>(*reinterpret_cast<const UA_UInt32*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_INT16])
            v = static_cast<double>(*reinterpret_cast<const UA_Int16*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_UINT16])
            v = static_cast<double>(*reinterpret_cast<const UA_UInt16*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_BYTE])
            v = static_cast<double>(*reinterpret_cast<const UA_Byte*>(tp_ua_ptr));
        else if (tp_ua_type == &UA_TYPES[UA_TYPES_SBYTE])
            v = static_cast<double>(*reinterpret_cast<const UA_SByte*>(tp_ua_ptr));

        if (v.has_value() && ((t_node.min_val_.has_value() && *v < t_node.min_val_.value()) ||
                                 (t_node.max_val_.has_value() && *v > t_node.max_val_.value()))) {
            return Error(OpcUaAdapterError::OUT_OF_RANGE);
        }
    }

    const sgrn::codecs::CodecEntry* entry = sgrn::codecs::codecEntryFor(t_node.type_);
    if (!entry)
        return Error(OpcUaAdapterError::CODEC_ENTRY_NOT_FOUND);

    s7codec::DecodedValue dv;
    if (!entry->from_ua(tp_ua_type, tp_ua_ptr, dv))
        return Error(OpcUaAdapterError::TYPE_MISMATCH);

    auto sc = s7codec::encodeScalar(dv, t_node.type_, tp_memory, s7_size, t_node.bit_index_, 0, t_node.endian_);
    if (!sc.has_value())
        return Error(OpcUaAdapterError::ENCODE_FAILED);
    return {};
} /**
   * @brief Recursively traverses a complex OPC UA ExtensionObject/Structure and packs it into S7 memory.
   *
   * Iterates through the UA_DataType members and matches them against the
   * nested PlcNode children defined by the SCL schema. Properly calculates
   * memory offsets and handles nested structures or primitive arrays.
   */
Result<void, OpcUaAdapterError> encodeStructOpcUaToMemory(
    const UA_DataType& t_type, const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const PlcNode& t_node) {
    size_t ua_offset = 0;
    for (size_t i = 0; i < t_type.membersSize; ++i) {
        const UA_DataTypeMember& m = t_type.members[i];
        if (i >= t_node.children_.size())
            break;
        const PlcNode& child = t_node.children_[i];
        ua_offset += m.padding;

        if (m.isArray) {
            const size_t count_ = *reinterpret_cast<const size_t*>(tp_ua_ptr + ua_offset);
            const uint8_t* p_array_data = *reinterpret_cast<const uint8_t* const*>(tp_ua_ptr + ua_offset + sizeof(size_t));
            if (p_array_data) {
                const size_t elem_stride = std::max<size_t>(1, child.size_);
                for (size_t j = 0; j < std::min(count_, (size_t)child.count_); ++j) {
                    const uint8_t* p_ua_elem_ptr = p_array_data + (j * m.memberType->memSize);
                    if (child.type_ == DataType::Bool) {
                        auto* p_bool_dest = tp_memory + child.offset_;
                        const auto* p_bool_array = reinterpret_cast<const UA_Boolean*>(p_array_data);
                        const size_t required_bytes = boolArrayByteCount(std::min(count_, (size_t)child.count_));
                        if (auto result =
                                writeBoolArrayToMemory(p_bool_array, std::min(count_, (size_t)child.count_), p_bool_dest, required_bytes);
                            result.hasError())
                            return result.error();
                        break;
                    }

                    uint8_t* p_s7_elem_ptr = tp_memory + child.offset_ + (j * elem_stride);
                    if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                        if (auto result = encodeStructOpcUaToMemory(*m.memberType, p_ua_elem_ptr, p_s7_elem_ptr, child); result.hasError())
                            return result.error();
                    } else {
                        PlcNode elem_node = child;
                        elem_node.count_ = 1;
                        elem_node.size_ = static_cast<uint32_t>(elem_stride);
                        elem_node.offset_ = 0;
                        if (auto result = encodeScalarOpcUaToMemory(m.memberType, p_ua_elem_ptr, p_s7_elem_ptr, elem_node);
                            result.hasError())
                            return result.error();
                    }
                }
            }
            ua_offset += sizeof(size_t) + sizeof(void*);
        } else {
            uint8_t* p_member = tp_memory + child.offset_;
            const uint8_t* p_ua_member_ptr = tp_ua_ptr + ua_offset;
            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                if (auto result = encodeStructOpcUaToMemory(*m.memberType, p_ua_member_ptr, p_member, child); result.hasError())
                    return result.error();
            } else {
                if (auto result = encodeScalarOpcUaToMemory(m.memberType, p_ua_member_ptr, p_member, child); result.hasError())
                    return result.error();
            }
            ua_offset += m.memberType->memSize;
        }
    }
    return {};
}

} // namespace sgrn::gateway::adapters
