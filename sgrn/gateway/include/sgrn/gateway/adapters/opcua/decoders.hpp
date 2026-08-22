#pragma once

#include <sgrn/Result.hpp>
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

struct RawDecodingContext {
    const NodeContext* p_node_ctx;
    const uint8_t* p_raw_data;
    size_t size;
    UA_DataValue* p_data_value;
};

/// Build a UA_DataValue from raw S7 field bytes using NodeContext type metadata.
Result<void, std::string> memoryBytesToDataValue(RawDecodingContext* tp_ctx);

Result<void, std::string> buildTypedArray(RawDecodingContext* tp_ctx);
/// Convert a decoded S7 scalar into a UA_DataValue using NodeContext type metadata.
/// Shared by read_handler.cpp and memory_to_ua.cpp to avoid duplicate type dispatch.

Result<void, std::string> setScalarFromDecoded(const s7codec::DecodedValue& t_dv, const NodeContext* tp_ctx, UA_DataValue* tp_data_value);

inline Result<UA_DateTime, std::string> decodeDtlBytesToUaDateTime(const uint8_t* tp_ptr, size_t t_size, s7codec::Endian t_endian) {
    if (!tp_ptr || t_size < 12)
        return Error("DTL must be 12");

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

/// Decode one S7 scalar field into an open62541 value buffer (member layout).
Result<void, std::string> decodeScalarToUa(
    const uint8_t* tp_memory_buf, const twin::PlcNode& t_node, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr);

/// Recursively project an S7 struct/array tree into a decoded UA struct buffer.
Result<void, std::string> decodeToOpcUa(
    const UA_DataType& t_type, const uint8_t* tp_memory_buf, uint8_t* tp_ua_ptr, const twin::PlcNode& t_node);

/// Build a UA ExtensionObject variant from live arena memory for a struct node.
Result<void, std::string> decodeStructObjectToExtensionObjectVariant(
    const twin::PlcNode& t_node, const UA_DataType& t_type, const ::sgrn::ArenaTree& t_arena, UA_Variant& t_out);

} // namespace sgrn::gateway::adapters
