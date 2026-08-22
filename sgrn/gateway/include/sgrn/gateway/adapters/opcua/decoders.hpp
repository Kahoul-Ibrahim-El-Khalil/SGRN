#pragma once

#include <sgrn/Result.hpp>
#include <cstddef>
#include <cstdint>
#include <open62541/server.h>
#include <s7codec/codec.hpp>
#include <string>
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

Result<void, std::string> setScalarFromDecoded(const s7codec::DecodedValue& t_dv, const NodeContext* tp_ctx, UA_DataValue* tp_data_value);
Result<void, std::string> buildTypedArray(RawDecodingContext* tp_ctx);
/// Convert a decoded S7 scalar into a UA_DataValue using NodeContext type metadata.
/// Shared by read_handler.cpp and memory_to_ua.cpp to avoid duplicate type dispatch.
bool setScalarFromDecoded(const s7codec::DecodedValue* tp_dv, const NodeContext* tp_ctx, UA_DataValue* tp_data_value);

} // namespace sgrn::gateway::adapters
