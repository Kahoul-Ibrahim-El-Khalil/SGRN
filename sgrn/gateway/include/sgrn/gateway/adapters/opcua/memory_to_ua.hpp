#pragma once

#include <cstddef>
#include <cstdint>
#include <open62541/server.h>
#include <s7codec/codec.hpp>
#include <string>

namespace sgrn::gateway::adapters
{

struct NodeContext;

/// Build a UA_DataValue from raw S7 field bytes using NodeContext type metadata.
bool s7BytesToDataValue(const NodeContext* tp_ctx, const uint8_t* tp_raw, size_t t_raw_size, UA_DataValue& t_dv);

/// Convert a decoded S7 scalar into a UA_DataValue using NodeContext type metadata.
/// Shared by read_handler.cpp and memory_to_ua.cpp to avoid duplicate type dispatch.
bool setScalarFromDecoded(const s7codec::DecodedValue& t_dv, const NodeContext* tp_ctx, UA_DataValue& t_data_value);

} // namespace sgrn::gateway::adapters
