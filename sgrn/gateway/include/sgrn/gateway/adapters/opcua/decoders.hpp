#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/gateway/adapters/opcua/scalar_view.hpp>
#include <cstddef>
#include <cstdint>
#include <open62541/server.h>
#include <open62541/types.h>
#include <s7codec/codec.hpp>
#include <string>
namespace sgrn::gateway::twin
{
struct PlcNode;
} // namespace sgrn::gateway::twin

namespace sgrn
{
class ArenaTree;
} // namespace sgrn

namespace sgrn::gateway::adapters
{

struct NodeContext;

/// Inputs shared by the decode functions: a schema NodeContext plus the raw S7
/// bytes to decode. The decoded UA_DataValue is *returned* rather than written
/// through an out-parameter, so there is no pre-allocated sink to manage.
struct OpcUaDecodingContext {
    const NodeContext* p_node_ctx;
    const uint8_t* p_raw_data;
    size_t size;
};

/// Build a UA_DataValue (scalar, temporal or typed array) from raw S7 field
/// bytes using NodeContext type metadata. This is the entry point shared by the
/// read handler and the delta-push path.
Result<UA_DataValue, OpcUaAdapterError> decodeMemoryBytesToDataValue(const OpcUaDecodingContext& t_ctx);

/// Decode a raw typed S7 array (incl. bool packing) into a UA_DataValue array.
Result<UA_DataValue, OpcUaAdapterError> decodeTypedArrayToDataValue(const OpcUaDecodingContext& t_ctx);

/// Convert a decoded S7 scalar into a UA_DataValue using NodeContext type
/// metadata. Shared by read_handler.cpp and the delta-push path to avoid
/// duplicate type dispatch.
Result<UA_DataValue, OpcUaAdapterError> decodeScalarToDataValue(const s7codec::DecodedValue& t_dv, const NodeContext& t_ctx);

inline Result<UA_DateTime, OpcUaAdapterError> decodeDtlBytesToUaDateTime(const uint8_t* tp_ptr, size_t t_size, s7codec::Endian t_endian) {
    if (!tp_ptr || t_size < 12)
        return Error(OpcUaAdapterError::INVALID_DTL);

    const uint32_t ns = s7codec::fromEndian<uint32_t>(tp_ptr + 8, t_endian);

    UA_DateTimeStruct dts{};
    dts.year = static_cast<UA_Int16>(s7codec::fromEndian<uint16_t>(tp_ptr, t_endian));
    dts.month = tp_ptr[2];
    dts.day = tp_ptr[3];
    dts.hour = tp_ptr[5];
    dts.min = tp_ptr[6];
    dts.sec = tp_ptr[7];
    dts.milliSec = static_cast<UA_UInt16>(ns / 1000000U);
    dts.microSec = static_cast<UA_UInt16>((ns / 1000U) % 1000U);
    dts.nanoSec = static_cast<UA_UInt16>(ns % 1000U);

    return UA_DateTime_fromStruct(dts);
}

/// Decode an S7 LDT or LDTL (8-byte ns since 1970) payload directly into a UA_DateTime.
inline Result<UA_DateTime, OpcUaAdapterError> decodeLdtBytesToUaDateTime(const uint8_t* tp_ptr, size_t t_size, s7codec::Endian t_endian) {
    if (!tp_ptr || t_size < 8)
        return Error(OpcUaAdapterError::INVALID_DTL);

    const int64_t ns_since_1970 = s7codec::fromEndian<int64_t>(tp_ptr, t_endian);
    // UA_DateTime is 100ns intervals since 1601-01-01.
    // 1970-01-01 is 11644473600 seconds after 1601-01-01.
    // So UA epoch offset = 11644473600 * 10000000 (100ns ticks).
    const int64_t ua_epoch_offset_100ns = 11644473600LL * 10000000LL;
    const int64_t ldt_100ns = ns_since_1970 / 100LL;
    return static_cast<UA_DateTime>(ua_epoch_offset_100ns + ldt_100ns);
}

/// Decode an S7 DATE_AND_TIME (8-byte BCD) payload directly into a UA_DateTime.
inline Result<UA_DateTime, OpcUaAdapterError> decodeDateAndTimeBytesToUaDateTime(const uint8_t* tp_ptr, size_t t_size) {
    if (!tp_ptr || t_size < 8)
        return Error(OpcUaAdapterError::INVALID_DTL);

    auto bcd = [](uint8_t b) -> uint8_t { return static_cast<uint8_t>(((b >> 4) * 10) + (b & 0x0F)); };

    const uint8_t year_bcd = bcd(tp_ptr[0]);
    UA_DateTimeStruct dts{};
    dts.year = static_cast<UA_Int16>(year_bcd + (year_bcd < 90 ? 2000 : 1900));
    dts.month = bcd(tp_ptr[1]);
    dts.day = bcd(tp_ptr[2]);
    dts.hour = bcd(tp_ptr[3]);
    dts.min = bcd(tp_ptr[4]);
    dts.sec = bcd(tp_ptr[5]);
    // Byte 6 = milliseconds BCD, byte 7 hi-nibble = tens-of-ms remainder (kept in ms).
    dts.milliSec = static_cast<UA_UInt16>((bcd(tp_ptr[6]) * 10) + (tp_ptr[7] >> 4));
    return UA_DateTime_fromStruct(dts);
}

/// @brief Decode any S7 temporal scalar type (DTL or DATE_AND_TIME) directly into
///        a UA_DateTime from its raw memory bytes — no string round-trip.
inline Result<UA_DateTime, OpcUaAdapterError> decodeTemporalBytesToUaDateTime(
    s7codec::Type t_type, const uint8_t* tp_ptr, size_t t_size, s7codec::Endian t_endian) {
    if (t_type == s7codec::Type::DTL)
        return decodeDtlBytesToUaDateTime(tp_ptr, t_size, t_endian);
    if (t_type == s7codec::Type::DateTime)
        return decodeDateAndTimeBytesToUaDateTime(tp_ptr, t_size);
    if (t_type == s7codec::Type::LDT || t_type == s7codec::Type::LDTL)
        return decodeLdtBytesToUaDateTime(tp_ptr, t_size, t_endian);
    return Error(OpcUaAdapterError::TYPE_MISMATCH);
}

/// Decode one S7 scalar field into an open62541 value buffer (member layout).
/// Takes a shallow PlcScalarView so array-element decoding doesn't need to
/// copy (or even reference) the full twin::PlcNode.
Result<void, OpcUaAdapterError> decodeScalarToUa(
    const uint8_t* tp_memory_buf, const PlcScalarView& t_view, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr);

/// Recursively project an S7 struct/array tree into a decoded UA struct buffer.
Result<void, OpcUaAdapterError> decodeToOpcUa(
    const UA_DataType& t_type, const uint8_t* tp_memory_buf, uint8_t* tp_ua_ptr, const twin::PlcNode& t_node);

/// Build a UA ExtensionObject variant from live arena memory for a struct node.
Result<UA_Variant, OpcUaAdapterError> decodeStructObjectToExtensionObjectVariant(
    const twin::PlcNode& t_node, const UA_DataType& t_type, const ::sgrn::ArenaTree& t_arena);

} // namespace sgrn::gateway::adapters
