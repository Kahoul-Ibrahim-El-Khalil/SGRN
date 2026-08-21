/**
 * @file write_handler.cpp
 * @brief Handles inbound OPC UA writes and translates them into S7 PLC memory payloads.
 *
 * This file acts as the primary data ingress point for the OPC UA protocol adapter.
 * When a remote client issues a Write request to the gateway, this file takes the
 * incoming UA_Variant payload, inspects its type, and attempts to map it to the
 * corresponding S7 PLC memory structure (PlcNode).
 *
 * If a native S7 mapping exists, it builds a high-performance binary payload and
 * pushes it directly to the PlcState command queue. If no native mapping exists,
 * it falls back to serializing the data into a generic JSON field.
 */
#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/udt_codec.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/core.h>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <vector>

using namespace sgrn::gateway::twin;
using ::sgrn::gateway::S7Area;
using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

// Placeholder: extract client IP from UA session when SessionRegistry hooks are wired up.
// Until installAccessControl is implemented, this returns "" (matching original behavior).
static std::string resolveSessionIp(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/) {
    return "";
}

// Placeholder: extract client certificate subject from UA session.
static std::string resolveCertSubject(UA_Server* /*server*/, const UA_NodeId* /*sessionId*/) {
    return "";
}

/**
 * @brief Converts an OPC UA DateTime (UTC) to a local time structure.
 *
 * OPC UA transmits timestamps strictly in UTC. If written directly to PLC blocks (like DTL),
 * the SCADA/HMI will display the raw UTC time, causing timezone offset mismatches (e.g., 1-hour drift).
 * This helper resolves the UA_DateTime into the OS's local timezone (respecting DST rules)
 * before it is serialized to JSON or packed into S7 binary memory.
 */
static struct tm resolveOpcUaLocalTime(UA_DateTime ua_date) {
    time_t t_sec = static_cast<time_t>(UA_DateTime_toUnixTime(ua_date) / 1000);
    struct tm tm_info;
    localtime_r(&t_sec, &tm_info);
    return tm_info;
}

template <size_t UaTypeIdx, typename UaType, typename EncodeFunc>
bool tryEncodePrimitive(const UA_DataType* tp_ua_type, const uint8_t* tp_ua_ptr, EncodeFunc func) {
    if (tp_ua_type == &UA_TYPES[UaTypeIdx]) {
        func(*reinterpret_cast<const UaType*>(tp_ua_ptr));
        return true;
    }
    return false;
}

static void encodeStringOpcUaToS7(const uint8_t* tp_ua_ptr, uint8_t* tp_s7_ptr, const PlcNode& t_node) {
    const auto* p_s = reinterpret_cast<const UA_String*>(tp_ua_ptr);
    if (t_node.type_ == DataType::String) {
        s7codec::encodeString(reinterpret_cast<const char*>(p_s->data), static_cast<int>(p_s->length), static_cast<int>(t_node.count_),
            tp_s7_ptr, t_node.size_);
    } else if (t_node.type_ == DataType::XString) {
        s7codec::encodeXString(reinterpret_cast<const char*>(p_s->data), static_cast<int>(p_s->length), static_cast<int>(t_node.count_),
            tp_s7_ptr, t_node.size_, t_node.endian_);
    } else if (t_node.type_ == DataType::WString || t_node.type_ == DataType::XWString) {
        std::string utf8(reinterpret_cast<const char*>(p_s->data), p_s->length);
        auto wide = sgrn::utils::strings::utf8ToUtf16(utf8);
        if (wide) {
            if (t_node.type_ == DataType::WString) {
                s7codec::encodeWString(reinterpret_cast<const uint16_t*>(wide->c_str()), static_cast<int>(wide->size()),
                    static_cast<int>(t_node.count_), tp_s7_ptr, t_node.size_, t_node.endian_);
            } else {
                s7codec::encodeXWString(reinterpret_cast<const uint16_t*>(wide->c_str()), static_cast<int>(wide->size()),
                    static_cast<int>(t_node.count_), tp_s7_ptr, t_node.size_, t_node.endian_);
            }
        }
    }
}

static UA_StatusCode encodeDateTimeOpcUaToS7(const uint8_t* tp_ua_ptr, uint8_t* tp_s7_ptr, const PlcNode& t_node, size_t s7_size) {
    const UA_DateTime ua_date = *reinterpret_cast<const UA_DateTime*>(tp_ua_ptr);
    const int64_t unix_time_sec = UA_DateTime_toUnixTime(ua_date) / 1000;
    if (t_node.type_ == DataType::DTL) {
        if (unix_time_sec < 0 || unix_time_sec > 9223372036LL)
            return UA_STATUSCODE_BADOUTOFRANGE;
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
        s7codec::encodeDtl(dtl, tp_s7_ptr, s7_size, t_node.endian_);
    } else if (t_node.type_ == DataType::DateTime) {
        if (unix_time_sec < 631152000LL)
            return UA_STATUSCODE_BADOUTOFRANGE;
        struct tm tm_info = resolveOpcUaLocalTime(ua_date);
        s7codec::encodeDateTime(tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min,
            tm_info.tm_sec, 0, tp_s7_ptr, s7_size);
    }
    return UA_STATUSCODE_GOOD;
}

/**
 * @brief Encodes a single scalar OPC UA primitive into a raw S7 binary buffer.
 *
 * Maps open62541 UA_TYPES to s7codec primitives. Handles endianness conversions,
 * bit alignments for booleans, and boundary validation for critical types like
 * DTL and DateTime, preventing out-of-range epoch underflows.
 */
UA_StatusCode encodeScalarOpcUaToS7(const UA_DataType* tp_ua_type, const uint8_t* tp_ua_ptr, uint8_t* tp_s7_ptr, const PlcNode& t_node) {
    size_t s7_size = s7codec::primitiveSize(t_node.type_);

    if (tryEncodePrimitive<UA_TYPES_BOOLEAN, UA_Boolean>(
            tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeBool(v, t_node.bit_index_, tp_s7_ptr, s7_size); })) {
    } else if (tryEncodePrimitive<UA_TYPES_SBYTE, UA_SByte>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeI8(v, tp_s7_ptr, s7_size); })) {
    } else if (tryEncodePrimitive<UA_TYPES_BYTE, UA_Byte>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeU8(v, tp_s7_ptr, s7_size); })) {
    } else if (tryEncodePrimitive<UA_TYPES_INT16, UA_Int16>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeI16(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_UINT16, UA_UInt16>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeU16(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_INT32, UA_Int32>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeI32(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_UINT32, UA_UInt32>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeU32(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_INT64, UA_Int64>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeI64(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_UINT64, UA_UInt64>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeU64(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_FLOAT, UA_Float>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeReal(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tryEncodePrimitive<UA_TYPES_DOUBLE, UA_Double>(
                   tp_ua_type, tp_ua_ptr, [&](auto v) { s7codec::encodeLReal(v, tp_s7_ptr, s7_size, t_node.endian_); })) {
    } else if (tp_ua_type == &UA_TYPES[UA_TYPES_STRING]) {
        encodeStringOpcUaToS7(tp_ua_ptr, tp_s7_ptr, t_node);
    } else if (tp_ua_type == &UA_TYPES[UA_TYPES_DATETIME]) {
        return encodeDateTimeOpcUaToS7(tp_ua_ptr, tp_s7_ptr, t_node, s7_size);
    }
    return UA_STATUSCODE_GOOD;
}

/**
 * @brief Recursively traverses a complex OPC UA ExtensionObject/Structure and packs it into S7 memory.
 *
 * Iterates through the UA_DataType members and matches them against the
 * nested PlcNode children defined by the SCL schema. Properly calculates
 * memory offsets and handles nested structures or primitive arrays.
 */
UA_StatusCode translateOpcUaToS7(const UA_DataType& t_type, const uint8_t* tp_ua_ptr, uint8_t* tp_s7_ptr, const PlcNode& t_node) {
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
                        auto* p_bool_dest = tp_s7_ptr + child.offset_;
                        const auto* p_bool_array = reinterpret_cast<const UA_Boolean*>(p_array_data);
                        const size_t required_bytes = boolArrayByteCount(std::min(count_, (size_t)child.count_));
                        if (!writeBoolArrayToS7(p_bool_array, std::min(count_, (size_t)child.count_), p_bool_dest, required_bytes))
                            return UA_STATUSCODE_BADINTERNALERROR;
                        break;
                    }

                    uint8_t* p_s7_elem_ptr = tp_s7_ptr + child.offset_ + (j * elem_stride);
                    if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                        UA_StatusCode sc = translateOpcUaToS7(*m.memberType, p_ua_elem_ptr, p_s7_elem_ptr, child);
                        if (sc != UA_STATUSCODE_GOOD)
                            return sc;
                    } else {
                        PlcNode elem_node = child;
                        elem_node.count_ = 1;
                        elem_node.size_ = static_cast<uint32_t>(elem_stride);
                        elem_node.offset_ = 0;
                        UA_StatusCode sc = encodeScalarOpcUaToS7(m.memberType, p_ua_elem_ptr, p_s7_elem_ptr, elem_node);
                        if (sc != UA_STATUSCODE_GOOD)
                            return sc;
                    }
                }
            }
            ua_offset += sizeof(size_t) + sizeof(void*);
        } else {
            uint8_t* p_s7_member_ptr = tp_s7_ptr + child.offset_;
            const uint8_t* p_ua_member_ptr = tp_ua_ptr + ua_offset;
            if (m.memberType->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                UA_StatusCode sc = translateOpcUaToS7(*m.memberType, p_ua_member_ptr, p_s7_member_ptr, child);
                if (sc != UA_STATUSCODE_GOOD)
                    return sc;
            } else {
                UA_StatusCode sc = encodeScalarOpcUaToS7(m.memberType, p_ua_member_ptr, p_s7_member_ptr, child);
                if (sc != UA_STATUSCODE_GOOD)
                    return sc;
            }
            ua_offset += m.memberType->memSize;
        }
    }
    return UA_STATUSCODE_GOOD;
}

// ====================================================================================
// TEMPLATE HELPERS FOR JSON SERIALIZATION
// These templates eliminate repetitive if-else blocks when serializing OPC UA Types.
// They use function objects (lambdas) to execute type-specific RapidJSON calls,
// mapping the UA_TYPES compile-time constants directly to their corresponding C++ types.
// ====================================================================================

template <size_t UaTypeIdx, typename T, typename WriteFunc>
bool trySerializeArray(const UA_Variant& v, rapidjson::Writer<rapidjson::StringBuffer>& w, WriteFunc func) {
    if (UA_Variant_hasArrayType(&v, &UA_TYPES[UaTypeIdx])) {
        const auto* arr = static_cast<const T*>(v.data);
        for (size_t i = 0; i < v.arrayLength; ++i) {
            func(w, arr[i]);
        }
        return true;
    }
    return false;
}

template <size_t UaTypeIdx, typename T, typename FormatFunc>
bool trySerializeScalar(const UA_Variant& t_value, std::string& t_out, FormatFunc t_func) {
    if (UA_Variant_hasScalarType(&t_value, &UA_TYPES[UaTypeIdx])) {
        t_out = t_func(*static_cast<const T*>(t_value.data));
        return true;
    }
    return false;
}

void serializeArrayToJson(const UA_Variant& t_value_opcua, rapidjson::Writer<rapidjson::StringBuffer>& t_writer) {
    t_writer.StartArray();

    if (trySerializeArray<UA_TYPES_DOUBLE, UA_Double>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Double(v); })) {
    } else if (trySerializeArray<UA_TYPES_FLOAT, UA_Float>(
                   t_value_opcua, t_writer, [](auto& w, auto v) { w.Double(static_cast<double>(v)); })) {
    } else if (trySerializeArray<UA_TYPES_INT32, UA_Int32>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Int(v); })) {
    } else if (trySerializeArray<UA_TYPES_UINT32, UA_UInt32>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Uint(v); })) {
    } else if (trySerializeArray<UA_TYPES_INT64, UA_Int64>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Int64(v); })) {
    } else if (trySerializeArray<UA_TYPES_UINT64, UA_UInt64>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Uint64(v); })) {
    } else if (trySerializeArray<UA_TYPES_INT16, UA_Int16>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Int(v); })) {
    } else if (trySerializeArray<UA_TYPES_UINT16, UA_UInt16>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Uint(v); })) {
    } else if (trySerializeArray<UA_TYPES_BYTE, UA_Byte>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Uint(v); })) {
    } else if (trySerializeArray<UA_TYPES_SBYTE, UA_SByte>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Int(v); })) {
    } else if (trySerializeArray<UA_TYPES_BOOLEAN, UA_Boolean>(t_value_opcua, t_writer, [](auto& w, auto v) { w.Bool(v != 0); })) {
    } else if (trySerializeArray<UA_TYPES_STRING, UA_String>(t_value_opcua, t_writer, [](auto& w, const auto& v) {
                   w.String(reinterpret_cast<const char*>(v.data), static_cast<rapidjson::SizeType>(v.length));
               })) {
    } else if (trySerializeArray<UA_TYPES_BYTESTRING, UA_ByteString>(t_value_opcua, t_writer, [](auto& w, const auto& v) {
                   w.String(reinterpret_cast<const char*>(v.data), static_cast<rapidjson::SizeType>(v.length));
               })) {
    } else if (trySerializeArray<UA_TYPES_DATETIME, UA_DateTime>(t_value_opcua, t_writer, [](auto& w, const auto& v) {
                   struct tm tm_info = resolveOpcUaLocalTime(v);
                   std::string p_s = fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                       tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
                   w.String(p_s.c_str());
               })) {
    } else if (UA_Variant_hasArrayType(&t_value_opcua, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]) ||
               (t_value_opcua.type && t_value_opcua.type->typeKind == UA_DATATYPEKIND_STRUCTURE)) {
        const bool is_eo = UA_Variant_hasArrayType(&t_value_opcua, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
        for (size_t i = 0; i < t_value_opcua.arrayLength; ++i) {
            const UA_DataType* p_udt_type_ptr = nullptr;
            const uint8_t* p_udt_data_ptr = nullptr;

            if (is_eo) {
                const auto* p_eo_arr = static_cast<const UA_ExtensionObject*>(t_value_opcua.data);
                const UA_ExtensionObject& p_eo = p_eo_arr[i];
                if ((p_eo.encoding == UA_EXTENSIONOBJECT_DECODED || p_eo.encoding == UA_EXTENSIONOBJECT_DECODED_NODELETE) &&
                    p_eo.content.decoded.data && p_eo.content.decoded.type) {
                    p_udt_type_ptr = p_eo.content.decoded.type;
                    p_udt_data_ptr = static_cast<const uint8_t*>(p_eo.content.decoded.data);
                }
            } else {
                p_udt_type_ptr = t_value_opcua.type;
                p_udt_data_ptr = static_cast<const uint8_t*>(t_value_opcua.data) + (i * t_value_opcua.type->memSize);
            }

            if (p_udt_type_ptr && p_udt_data_ptr) {
                rapidjson::Document doc;
                rapidjson::Value jv_elem;
                if (deserializeMemoryToUdtJson(p_udt_data_ptr, *p_udt_type_ptr, jv_elem, doc.GetAllocator())) {
                    jv_elem.Accept(t_writer);
                } else {
                    t_writer.Null();
                }
            } else {
                t_writer.Null();
            }
        }
    }
    t_writer.EndArray();
}

std::string serializeScalarToJson(const UA_Variant& t_value, const NodeContext* tp_ctx) {
    std::string out;
    if (trySerializeScalar<UA_TYPES_DOUBLE, UA_Double>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_FLOAT, UA_Float>(t_value, out, [](auto v) { return fmt::format("{}", static_cast<double>(v)); }))
        return out;
    if (trySerializeScalar<UA_TYPES_INT32, UA_Int32>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_UINT32, UA_UInt32>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_INT64, UA_Int64>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_UINT64, UA_UInt64>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_INT16, UA_Int16>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_UINT16, UA_UInt16>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_BYTE, UA_Byte>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_SBYTE, UA_SByte>(t_value, out, [](auto v) { return fmt::format("{}", v); }))
        return out;
    if (trySerializeScalar<UA_TYPES_BOOLEAN, UA_Boolean>(t_value, out, [](auto v) { return v ? "true" : "false"; }))
        return out;

    if (UA_Variant_hasScalarType(&t_value, &UA_TYPES[UA_TYPES_STRING]) ||
        UA_Variant_hasScalarType(&t_value, &UA_TYPES[UA_TYPES_BYTESTRING])) {
        const auto* p_s = static_cast<const UA_String*>(t_value.data);
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> t_w(sb);
        t_w.String(reinterpret_cast<const char*>(p_s->data), static_cast<rapidjson::SizeType>(p_s->length));
        return sb.GetString();
    } else if (UA_Variant_hasScalarType(&t_value, &UA_TYPES[UA_TYPES_DATETIME])) {
        struct tm tm_info = resolveOpcUaLocalTime(*static_cast<const UA_DateTime*>(t_value.data));
        return fmt::format("\"{:04}-{:02}-{:02} {:02}:{:02}:{:02}\"", tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else if (!tp_ctx->udt_name.empty() || (t_value.type && t_value.type->typeKind == UA_DATATYPEKIND_STRUCTURE)) {
        const UA_DataType* p_udt_type_ptr = nullptr;
        const uint8_t* p_udt_data_ptr = nullptr;

        if (UA_Variant_hasScalarType(&t_value, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT])) {
            const auto* p_eo = static_cast<const UA_ExtensionObject*>(t_value.data);
            if ((p_eo->encoding == UA_EXTENSIONOBJECT_DECODED || p_eo->encoding == UA_EXTENSIONOBJECT_DECODED_NODELETE) &&
                p_eo->content.decoded.data && p_eo->content.decoded.type) {
                p_udt_type_ptr = p_eo->content.decoded.type;
                p_udt_data_ptr = static_cast<const uint8_t*>(p_eo->content.decoded.data);
            }
        } else if (t_value.type && t_value.type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
            p_udt_type_ptr = t_value.type;
            p_udt_data_ptr = static_cast<const uint8_t*>(t_value.data);
        }

        if (p_udt_type_ptr && p_udt_data_ptr) {
            rapidjson::Document doc;
            rapidjson::Value jv_obj;
            if (deserializeMemoryToUdtJson(p_udt_data_ptr, *p_udt_type_ptr, jv_obj, doc.GetAllocator())) {
                rapidjson::StringBuffer sb;
                rapidjson::Writer<rapidjson::StringBuffer> t_w(sb);
                jv_obj.Accept(t_w);
                return sb.GetString();
            }
        }
    }
    return "";
}

/**
 * @brief Attempts to execute a high-performance native binary write to the PLC.
 *
 * This is the "fast path". It resolves the raw pointers from the OPC UA variant,
 * allocates a correctly sized binary buffer based on the schema (PlcNode), and packs
 * the data using `encodeScalarOpcUaToS7` or `translateOpcUaToS7`. The resulting binary
 * payload is then wrapped in a PlcCommand and queued for execution by the PlcCommandProcessor.
 *
 * @return UA_STATUSCODE_GOOD on success, an error code on failure, or std::nullopt
 *         if the data type cannot be natively mapped (triggering a JSON fallback).
 */
std::optional<UA_StatusCode> tryBinaryWrite(NodeContext* p_ctx, const UA_Variant& t_v, const PlcNode* p_node) {
    const UA_DataType* p_udt_type = nullptr;
    const uint8_t* p_udt_data = nullptr;
    bool is_array_input = (t_v.arrayLength > 0);

    if (is_array_input) {
        if (t_v.arrayLength == p_ctx->array_length || (p_node->is_dynamic_ && t_v.arrayLength <= p_ctx->array_length)) {
            p_udt_type = t_v.type;
            p_udt_data = static_cast<const uint8_t*>(t_v.data);
        }
    } else {
        if (UA_Variant_hasScalarType(&t_v, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT])) {
            const auto* p_eo = static_cast<const UA_ExtensionObject*>(t_v.data);
            if ((p_eo->encoding == UA_EXTENSIONOBJECT_DECODED || p_eo->encoding == UA_EXTENSIONOBJECT_DECODED_NODELETE) &&
                p_eo->content.decoded.data && p_eo->content.decoded.type) {
                p_udt_type = p_eo->content.decoded.type;
                p_udt_data = static_cast<const uint8_t*>(p_eo->content.decoded.data);
            }
        } else if (t_v.type) {
            p_udt_type = t_v.type;
            p_udt_data = static_cast<const uint8_t*>(t_v.data);
        }
    }

    if (!p_udt_type || !p_udt_data)
        return std::nullopt;

    const size_t element_span = std::max<size_t>(1, p_node->size_);
    const size_t input_count = is_array_input ? std::min(t_v.arrayLength, static_cast<size_t>(p_ctx->array_length)) : 1U;
    const size_t header_offset = p_node->is_dynamic_ ? 4U : 0U;
    const bool is_bool_array =
        is_array_input && p_node->type_ == DataType::Bool && UA_Variant_hasArrayType(&t_v, &UA_TYPES[UA_TYPES_BOOLEAN]);
    const size_t payload_size = is_bool_array ? boolArrayByteCount(input_count) : (element_span * input_count);
    std::vector<uint8_t> s7_binary(header_offset + payload_size, 0);

    if (p_node->is_dynamic_)
        s7codec::toEndian<uint32_t>(static_cast<uint32_t>(input_count), s7_binary.data(), p_node->endian_);

    if (is_array_input) {
        if (is_bool_array) {
            const auto* p_bools = reinterpret_cast<const UA_Boolean*>(p_udt_data);
            if (!writeBoolArrayToS7(p_bools, input_count, s7_binary.data() + header_offset, s7_binary.size() - header_offset))
                return UA_STATUSCODE_BADINTERNALERROR;
        } else {
            const bool is_eo_arr = UA_Variant_hasArrayType(&t_v, &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]);
            for (size_t j = 0; j < input_count; ++j) {
                uint8_t* p_s7_elem_ptr = s7_binary.data() + header_offset + (j * element_span);
                const UA_DataType* p_current_udt_type = p_udt_type;
                const uint8_t* p_current_ua_data = p_udt_data + (j * p_udt_type->memSize);

                if (is_eo_arr) {
                    const auto* p_eo_arr = static_cast<const UA_ExtensionObject*>(t_v.data);
                    const UA_ExtensionObject& p_eo = p_eo_arr[j];
                    if ((p_eo.encoding == UA_EXTENSIONOBJECT_DECODED || p_eo.encoding == UA_EXTENSIONOBJECT_DECODED_NODELETE) &&
                        p_eo.content.decoded.data && p_eo.content.decoded.type) {
                        p_current_udt_type = p_eo.content.decoded.type;
                        p_current_ua_data = static_cast<const uint8_t*>(p_eo.content.decoded.data);
                    }
                }

                if (!p_current_udt_type || !p_current_ua_data)
                    continue;

                if (p_current_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
                    UA_StatusCode sc = translateOpcUaToS7(*p_current_udt_type, p_current_ua_data, p_s7_elem_ptr, *p_node);
                    if (sc != UA_STATUSCODE_GOOD)
                        return sc;
                } else {
                    PlcNode elem_node = *p_node;
                    if (p_node->type_ == DataType::String || p_node->type_ == DataType::WString || p_node->type_ == DataType::XString ||
                        p_node->type_ == DataType::XWString) {
                        if (p_node->type_ == DataType::String || p_node->type_ == DataType::XString) {
                            elem_node.count_ = (element_span >= 2) ? static_cast<uint32_t>(element_span - 2) : 0;
                        } else {
                            elem_node.count_ = (element_span >= 4) ? static_cast<uint32_t>((element_span - 4) / 2) : 0;
                        }
                    } else {
                        elem_node.count_ = 1;
                    }
                    elem_node.size_ = static_cast<uint32_t>(element_span);
                    elem_node.offset_ = 0;
                    UA_StatusCode sc = encodeScalarOpcUaToS7(p_current_udt_type, p_current_ua_data, p_s7_elem_ptr, elem_node);
                    if (sc != UA_STATUSCODE_GOOD)
                        return sc;
                }
            }
        }
    } else if (p_udt_type->typeKind == UA_DATATYPEKIND_STRUCTURE) {
        UA_StatusCode sc = translateOpcUaToS7(*p_udt_type, p_udt_data, s7_binary.data() + header_offset, *p_node);
        if (sc != UA_STATUSCODE_GOOD)
            return sc;
    } else if (is_bool_array) {
        const auto* p_bools = static_cast<const UA_Boolean*>(t_v.data);
        if (!writeBoolArrayToS7(p_bools, t_v.arrayLength, s7_binary.data() + header_offset, s7_binary.size() - header_offset))
            return UA_STATUSCODE_BADINTERNALERROR;
    } else {
        UA_StatusCode sc = encodeScalarOpcUaToS7(p_udt_type, p_udt_data, s7_binary.data() + header_offset, *p_node);
        if (sc != UA_STATUSCODE_GOOD)
            return sc;
    }

    PlcCommand cmd;
    cmd.type = PlcCommand::WriteBinary;
    cmd.db_number = p_ctx->db_number;
    cmd.size = s7_binary.size();
    cmd.offset = p_node->offset_ + (is_array_input ? 0 : 0);

    auto* p_entry = p_ctx->server->state()->findSegmentById(p_ctx->db_number);

    cmd.path = (p_entry ? p_entry->name : "") + "." + p_ctx->field_path;

    cmd.value_binary = std::move(s7_binary);

    cmd.timestamp = sgrn::utils::time::nowMilliseconds();

    p_ctx->server->state()->pushCommand(std::move(cmd));

    p_ctx->server->signalDirty();

    return UA_STATUSCODE_GOOD;
}

/**
 * @brief Fallback mechanism for handling unmapped or schema-less OPC UA writes.
 *
 * If the target variable does not have a strict S7 binary memory layout (e.g., dynamically
 * created nodes or raw string blobs), this function serializes the UA_Variant into a JSON
 * string and pushes it to the generic runtime state instead of the S7 native memory block.
 */
UA_StatusCode tryJsonWrite(NodeContext* p_ctx, const UA_Variant& t_v) {
    std::string json_val;
    if (p_ctx->array_length > 0 && t_v.arrayLength > 0) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> t_w(sb);
        serializeArrayToJson(t_v, t_w);
        json_val = sb.GetString();
    } else {
        json_val = serializeScalarToJson(t_v, p_ctx);
    }

    if (json_val.empty())
        return UA_STATUSCODE_BADDATATYPEIDUNKNOWN;

    auto res = p_ctx->server->updateField(p_ctx->db_number, p_ctx->field_path, json_val);
    return res.hasError() ? UA_STATUSCODE_BADWRITENOTSUPPORTED : UA_STATUSCODE_GOOD;
}

/**
 * @brief The main OPC UA write interceptor callback registered with the open62541 server.
 *
 * Orchestrates the entire write pipeline:
 * 1. Validates session security and access rights.
 * 2. Attempts the high-performance native binary write (tryBinaryWrite).
 * 3. Falls back to JSON serialization (tryJsonWrite) if necessary.
 */
UA_StatusCode writeS7Value(UA_Server* tp_ua_server, const UA_NodeId* tp_session_id, void* /*sessionContext*/, const UA_NodeId* /*nodeId*/,
    void* tp_node_context, const UA_NumericRange* /*range*/, const UA_DataValue* tp_data_value) {

    auto* p_ctx = static_cast<NodeContext*>(tp_node_context);
    if (!p_ctx || !p_ctx->server || !p_ctx->security)
        return UA_STATUSCODE_BADINTERNALERROR;

    const std::string client_ip = resolveSessionIp(tp_ua_server, tp_session_id);
    const std::string cert_subject = resolveCertSubject(tp_ua_server, tp_session_id);

    if (!p_ctx->security->authorizeField(
            security::Protocol::OpcUA, client_ip, p_ctx->db_number, p_ctx->field_path, true, "", {}, cert_subject))
        return UA_STATUSCODE_BADUSERACCESSDENIED;

    if (!tp_data_value || !tp_data_value->hasValue)
        return UA_STATUSCODE_BADNODATA;

    const UA_Variant& t_v = tp_data_value->value;
    const PlcNode* p_node = p_ctx->server->findSymbol(p_ctx->db_number, p_ctx->field_path);

    if (p_node) {
        auto opt_sc = tryBinaryWrite(p_ctx, t_v, p_node);
        if (opt_sc.has_value())
            return opt_sc.value();
    }

    return tryJsonWrite(p_ctx, t_v);
}

} // namespace sgrn::gateway::adapters
