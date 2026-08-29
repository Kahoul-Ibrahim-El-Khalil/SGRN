#include <sgrn/gateway/adapters/opcua/encoders.hpp>

#include <cstdio>
#include <limits>

namespace sgrn::gateway::adapters
{

// tryEncodePrimitive removed — dispatch is now table-driven via
// sgrn::codecs::kCodecTable.
using s7codec::encodeString;
using s7codec::encodeWString;
using s7codec::encodeXString;
using s7codec::encodeXWString;
using scl::DataType;
using sgrn::gateway::twin::PlcNode;

OpcUaEncodingContext::OpcUaEncodingContext(
    const UA_DataType* tp_ua_type, const uint8_t* tp_ua_buf, uint8_t* tp_memory, const twin::PlcNode* tp_node)
    : p_ua_type(tp_ua_type)
    , p_ua_buf(tp_ua_buf)
    , p_memory(tp_memory)
    , view{}
    , p_node(tp_node) {

    if (tp_node)
        view = makeScalarView(*tp_node);
}

size_t boolArrayByteCount(size_t t_bit_count) {
    return (t_bit_count + 7U) / 8U;
}

Result<void, OpcUaAdapterError> encodeArrayOfBoolsToMemory(const ArrayOfBoolsEncodingContext& t_ctx) {

    for (size_t index = 0; index < t_ctx.count; ++index) {
        const size_t byte_index = index / 8U;
        SGRN_RETURN_ERROR_IF(byte_index >= t_ctx.destination_size, OpcUaAdapterError::OUT_OF_RANGE);

        SGRN_RETURN_IF(!s7codec::encodeBool(t_ctx.p_source[index] != 0, static_cast<int>(index % 8U), t_ctx.p_destination + byte_index,
                           t_ctx.destination_size - byte_index)
                           .has_value(),
            OpcUaAdapterError::ENCODE_FAILED);
    }
    return {};
}

Result<void, OpcUaAdapterError> encodeStringOpcUaToMemory(const OpcUaEncodingContext& t_ctx) {

    SGRN_RETURN_IF_NULL(t_ctx.p_ua_buf, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_IF_NULL(t_ctx.p_memory, OpcUaAdapterError::NULL_POINTER);

    const auto* p_string = reinterpret_cast<const UA_String*>(t_ctx.p_ua_buf);

    SGRN_RETURN_IF_NULL(p_string->data, OpcUaAdapterError::TYPE_MISMATCH);

    const auto p_string_data_char = reinterpret_cast<const char*>(p_string->data);

    const int string_length = static_cast<int>(p_string->length);

    const uint32_t string_capacity = t_ctx.view.string_capacity;

    if (t_ctx.view.type == DataType::String) {
        SGRN_RETURN_IF(!encodeString(p_string_data_char, string_length, string_capacity, t_ctx.p_memory, t_ctx.effectiveSize()).has_value(),
            OpcUaAdapterError::OUT_OF_RANGE);

    } else if (t_ctx.view.type == DataType::XString) {
        SGRN_RETURN_IF(
            !encodeXString(p_string_data_char, string_length, string_capacity, t_ctx.p_memory, t_ctx.effectiveSize(), t_ctx.view.endian)
                .has_value(),
            OpcUaAdapterError::OUT_OF_RANGE);

    } else if (t_ctx.view.type == DataType::WString || t_ctx.view.type == DataType::XWString) {

        std::string utf8(p_string_data_char, string_length);

        std::optional<std::u16string> wide = sgrn::utils::strings::utf8ToUtf16(utf8);

        SGRN_RETURN_IF_NULL(wide, OpcUaAdapterError::TYPE_MISMATCH);

        const uint16_t* p_string_wide_char = reinterpret_cast<const uint16_t*>(wide->c_str());

        const int wide_string_length = static_cast<int>(wide->size());

        if (t_ctx.view.type == DataType::WString) {
            SGRN_RETURN_IF(!encodeWString(p_string_wide_char, wide_string_length, string_capacity, t_ctx.p_memory, t_ctx.effectiveSize(),
                               t_ctx.view.endian)
                               .has_value(),
                OpcUaAdapterError::OUT_OF_RANGE);
        } else {
            SGRN_RETURN_IF(!encodeXWString(p_string_wide_char, wide_string_length, string_capacity, t_ctx.p_memory, t_ctx.effectiveSize(),
                               t_ctx.view.endian)
                               .has_value(),
                OpcUaAdapterError::OUT_OF_RANGE);
        }
    }

    return {};
}

Result<void, OpcUaAdapterError> encodeTimeOfDayOpcUaToMemory(const OpcUaEncodingContext& tp_ctx) {

    SGRN_RETURN_IF_NULL(tp_ctx.p_ua_buf, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_IF_NULL(tp_ctx.p_memory, OpcUaAdapterError::NULL_POINTER);

    // NOTE: p_node is intentionally null on the per-array-element scalar
    // path (tryBinaryWrite builds a PlcScalarView there instead of a full
    // PlcNode, to avoid deep-copying name_/children_/atomic per element —
    // see writeValue.cpp). Don't require p_node; view.endian already
    // mirrors p_node->endian_ in both OpcUaEncodingContext constructors.
    SGRN_RETURN_ERROR_IF(tp_ctx.view.type != DataType::TimeOfDay, OpcUaAdapterError::TYPE_MISMATCH);

    const auto* ua_string = reinterpret_cast<const UA_String*>(tp_ctx.p_ua_buf);

    SGRN_RETURN_IF_NULL(ua_string->data, OpcUaAdapterError::TYPE_MISMATCH);

    const std::string value(reinterpret_cast<const char*>(ua_string->data), ua_string->length);

    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    unsigned millisecond = 0;

    char trailing = '\0';

    const int parsed = std::sscanf(value.c_str(), "%u:%u:%u.%u%c", &hour, &minute, &second, &millisecond, &trailing);

    SGRN_RETURN_ERROR_IF(parsed < 3 || parsed > 4, OpcUaAdapterError::TYPE_MISMATCH);

    SGRN_RETURN_ERROR_IF(hour > 23 || minute > 59 || second > 59, OpcUaAdapterError::OUT_OF_RANGE);

    SGRN_RETURN_ERROR_IF(millisecond > 999, OpcUaAdapterError::OUT_OF_RANGE);

    const uint64_t total_ms = (((static_cast<uint64_t>(hour) * 60ULL) + minute) * 60ULL + second) * 1000ULL + millisecond;

    SGRN_RETURN_ERROR_IF(total_ms > std::numeric_limits<uint32_t>::max(), OpcUaAdapterError::OUT_OF_RANGE);

    s7codec::toEndian<uint32_t>(static_cast<uint32_t>(total_ms), tp_ctx.p_memory, tp_ctx.view.endian);

    return {};
}
Result<void, OpcUaAdapterError> encodeDateTimeOpcUaToMemory(const OpcUaEncodingContext& t_ctx, size_t s7_size) {

    SGRN_RETURN_IF_NULL(t_ctx.p_ua_buf, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_IF_NULL(t_ctx.p_memory, OpcUaAdapterError::NULL_POINTER);

    const UA_DateTime ua_date = *reinterpret_cast<const UA_DateTime*>(t_ctx.p_ua_buf);

    // UA_DateTime_toUnixTime() returns Unix time in seconds.
    const int64_t unix_time_sec = UA_DateTime_toUnixTime(ua_date);

    if (t_ctx.view.type == DataType::DTL) {
        SGRN_RETURN_ERROR_IF(unix_time_sec < 0 || unix_time_sec > 9223372036LL, OpcUaAdapterError::OUT_OF_RANGE);

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

        SGRN_RETURN_IF(!s7codec::encodeDtl(dtl, t_ctx.p_memory, s7_size, t_ctx.view.endian).has_value(), OpcUaAdapterError::ENCODE_FAILED);

    } else if (t_ctx.view.type == DataType::DateTime) {
        SGRN_RETURN_ERROR_IF(unix_time_sec < 631152000LL, OpcUaAdapterError::OUT_OF_RANGE);

        struct tm tm_info = resolveOpcUaLocalTime(ua_date);

        SGRN_RETURN_IF(!s7codec::encodeDateTime(tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour,
                           tm_info.tm_min, tm_info.tm_sec, 0, t_ctx.p_memory, s7_size)
                           .has_value(),
            OpcUaAdapterError::ENCODE_FAILED);

    } else if (t_ctx.view.type == DataType::LDT || t_ctx.view.type == DataType::LDTL) {

        // UA_DateTime is 100ns intervals since 1601-01-01.
        // LDT is ns since 1970-01-01.
        constexpr int64_t ua_epoch_offset_100ns = 11644473600LL * 10000000LL;

        SGRN_RETURN_ERROR_IF(ua_date < ua_epoch_offset_100ns, OpcUaAdapterError::OUT_OF_RANGE);

        const int64_t ldt_100ns = ua_date - ua_epoch_offset_100ns;

        const int64_t ns_since_1970 = ldt_100ns * 100LL;

        s7codec::toEndian<int64_t>(ns_since_1970, t_ctx.p_memory, t_ctx.view.endian);
    }

    return {};
}

Result<void, OpcUaAdapterError> encodeScalarOpcUaToMemory(const OpcUaEncodingContext& t_ctx) {

    SGRN_RETURN_IF_NULL(t_ctx.p_ua_buf, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_IF_NULL(t_ctx.p_memory, OpcUaAdapterError::NULL_POINTER);

    const size_t s7_size = [&]() -> size_t {
        auto opt = s7codec::primitiveSize(t_ctx.view.type);
        return opt ? static_cast<size_t>(*opt) : 0;
    }();

    if (t_ctx.p_ua_type && t_ctx.p_ua_type->typeKind == UA_DATATYPEKIND_ENUM) {

        const UA_Int32 raw = *reinterpret_cast<const UA_Int32*>(t_ctx.p_ua_buf);

        s7codec::DecodedValue dv = s7codec::DecodedValue::makeSigned(static_cast<int64_t>(raw));

        SGRN_RETURN_IF(
            !s7codec::encodeScalar(dv, t_ctx.view.type, t_ctx.p_memory, s7_size, t_ctx.view.bit_index, 0, t_ctx.view.endian).has_value(),
            OpcUaAdapterError::ENCODE_FAILED);

        return {};
    }

    if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_STRING]) {
        if (t_ctx.view.type == DataType::TimeOfDay)
            return encodeTimeOfDayOpcUaToMemory(t_ctx);

        return encodeStringOpcUaToMemory(t_ctx);
    }

    if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_DATETIME])
        return encodeDateTimeOpcUaToMemory(t_ctx, s7_size);

    // Engineering-range guard.
    // This function is the common scalar encoding path for scalar leaves,
    // array elements, and structure members.
    if (t_ctx.view.min_val.has_value() || t_ctx.view.max_val.has_value()) {

        std::optional<double> v;

        if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_DOUBLE])
            v = *reinterpret_cast<const UA_Double*>(t_ctx.p_ua_buf);
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_FLOAT])
            v = static_cast<double>(*reinterpret_cast<const UA_Float*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_INT64])
            v = static_cast<double>(*reinterpret_cast<const UA_Int64*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_UINT64])
            v = static_cast<double>(*reinterpret_cast<const UA_UInt64*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_INT32])
            v = static_cast<double>(*reinterpret_cast<const UA_Int32*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_UINT32])
            v = static_cast<double>(*reinterpret_cast<const UA_UInt32*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_INT16])
            v = static_cast<double>(*reinterpret_cast<const UA_Int16*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_UINT16])
            v = static_cast<double>(*reinterpret_cast<const UA_UInt16*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_BYTE])
            v = static_cast<double>(*reinterpret_cast<const UA_Byte*>(t_ctx.p_ua_buf));
        else if (t_ctx.p_ua_type == &UA_TYPES[UA_TYPES_SBYTE])
            v = static_cast<double>(*reinterpret_cast<const UA_SByte*>(t_ctx.p_ua_buf));

        if (v.has_value()) {
            SGRN_RETURN_ERROR_IF((t_ctx.view.min_val.has_value() && *v < *t_ctx.view.min_val) ||
                                     (t_ctx.view.max_val.has_value() && *v > *t_ctx.view.max_val),
                OpcUaAdapterError::OUT_OF_RANGE);
        }
    }

    const sgrn::codecs::CodecEntry* p_entry = sgrn::codecs::codecEntryFor(t_ctx.view.type);

    SGRN_RETURN_IF_NULL(p_entry, OpcUaAdapterError::CODEC_ENTRY_NOT_FOUND);

    s7codec::DecodedValue dv;

    SGRN_RETURN_IF(!p_entry->from_ua(t_ctx.p_ua_type, t_ctx.p_ua_buf, dv), OpcUaAdapterError::TYPE_MISMATCH);

    SGRN_RETURN_IF(
        !s7codec::encodeScalar(dv, t_ctx.view.type, t_ctx.p_memory, s7_size, t_ctx.view.bit_index, 0, t_ctx.view.endian).has_value(),
        OpcUaAdapterError::ENCODE_FAILED);

    return {};
}

/**
 * @brief Recursively traverses a complex OPC UA ExtensionObject/Structure
 *        and packs it into S7 memory.
 *
 * Iterates through the UA_DataType members and matches them against the
 * nested PlcNode children defined by the SCL schema. Properly calculates
 * memory offsets and handles nested structures or primitive arrays.
 */
namespace
{
constexpr uint16_t kMaxStructDepth = 32;
}

Result<void, OpcUaAdapterError> encodeStructOpcUaToMemory(const OpcUaEncodingContext& tp_ctx) {
    SGRN_RETURN_IF_NULL(tp_ctx.p_node, OpcUaAdapterError::NULL_POINTER);
    SGRN_RETURN_IF_NULL(tp_ctx.p_ua_type, OpcUaAdapterError::NULL_POINTER);
    SGRN_RETURN_IF_NULL(tp_ctx.p_ua_buf, OpcUaAdapterError::NULL_POINTER);
    SGRN_RETURN_IF_NULL(tp_ctx.p_memory, OpcUaAdapterError::NULL_POINTER);

    SGRN_RETURN_ERROR_IF(tp_ctx.depth >= kMaxStructDepth, OpcUaAdapterError::OUT_OF_RANGE);

    // p_memory always points at the start of a region exactly this many
    // bytes long — every child offset/span below must fit inside it.
    const size_t node_span = std::max<size_t>(1, tp_ctx.p_node->size_);

    size_t ua_offset = 0;

    for (size_t i = 0; i < tp_ctx.p_ua_type->membersSize; ++i) {
        const UA_DataTypeMember& m = tp_ctx.p_ua_type->members[i];
        if (i >= tp_ctx.p_node->children_.size())
            break;

        const PlcNode& child = tp_ctx.p_node->children_[i];
        ua_offset += m.padding;

        const size_t child_own_span = std::max<size_t>(1, child.size_);
        SGRN_RETURN_ERROR_IF(child.offset_ + child_own_span > node_span, OpcUaAdapterError::OUT_OF_RANGE);

        if (m.isArray) {
            const size_t count_ = *reinterpret_cast<const size_t*>(tp_ctx.p_ua_buf + ua_offset);
            const uint8_t* p_array_data = *reinterpret_cast<const uint8_t* const*>(tp_ctx.p_ua_buf + ua_offset + sizeof(size_t));

            if (p_array_data) {
                const size_t elem_stride = std::max<size_t>(1, child.size_);
                const size_t count = std::min(count_, static_cast<size_t>(child.count_));

                SGRN_RETURN_ERROR_IF(child.offset_ + count * elem_stride > node_span, OpcUaAdapterError::OUT_OF_RANGE);

                for (size_t j = 0; j < count; ++j) {
                    const uint8_t* p_ua_elem_ptr = p_array_data + (j * m.memberType->memSize);

                    if (child.type_ == DataType::Bool) {

                        const size_t required_bytes = boolArrayByteCount(count);
                        SGRN_RETURN_ERROR_IF(child.offset_ + required_bytes > node_span, OpcUaAdapterError::OUT_OF_RANGE);

                        const ArrayOfBoolsEncodingContext ctx{.p_source = reinterpret_cast<const UA_Boolean*>(p_array_data),
                            .count = count,
                            .p_destination = tp_ctx.p_memory + child.offset_,
                            .destination_size = required_bytes};

                        SGRN_IF_ERROR_PROPAGATE(encodeArrayOfBoolsToMemory(ctx));
                        break;
                    }

                    uint8_t* p_s7_elem_ptr = tp_ctx.p_memory + child.offset_ + (j * elem_stride);
                    OpcUaEncodingContext elem_ctx{m.memberType, p_ua_elem_ptr, p_s7_elem_ptr, &child};
                    elem_ctx.depth = tp_ctx.depth + 1;

                    if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                        SGRN_IF_ERROR_PROPAGATE(encodeStructOpcUaToMemory(elem_ctx));
                    } else {
                        elem_ctx.override_size = static_cast<uint32_t>(elem_stride);
                        SGRN_IF_ERROR_PROPAGATE(encodeScalarOpcUaToMemory(elem_ctx));
                    }
                }
            }
            ua_offset += sizeof(size_t) + sizeof(void*);
        } else {
            uint8_t* p_member = tp_ctx.p_memory + child.offset_;
            const uint8_t* p_ua_member_ptr = tp_ctx.p_ua_buf + ua_offset;
            OpcUaEncodingContext elem_ctx{m.memberType, p_ua_member_ptr, p_member, &child};
            elem_ctx.depth = tp_ctx.depth + 1;

            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                SGRN_IF_ERROR_PROPAGATE(encodeStructOpcUaToMemory(elem_ctx));
            } else {
                SGRN_IF_ERROR_PROPAGATE(encodeScalarOpcUaToMemory(elem_ctx));
            }
            ua_offset += m.memberType->memSize;
        }
    }
    return {};
}
} // namespace sgrn::gateway::adapters
