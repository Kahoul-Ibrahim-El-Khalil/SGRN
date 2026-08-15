#pragma once

#include <cstddef>
#include <cstdint>
#include <open62541/types.h>

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

/// Decode one S7 scalar field into an open62541 value buffer (member layout).
bool decodeScalarS7ToUa(const uint8_t* tp_s7_ptr, const twin::PlcNode& t_node, const UA_DataType* tp_ua_type, uint8_t* tp_ua_ptr);

/// Recursively project an S7 struct/array tree into a decoded UA struct buffer.
bool translateS7ToOpcUa(const UA_DataType& t_type, const uint8_t* tp_s7_ptr, uint8_t* tp_ua_ptr, const twin::PlcNode& t_node);

/// Build a UA ExtensionObject variant from live arena memory for a struct node.
bool s7StructToExtensionObjectVariant(
    const twin::PlcNode& t_node, const UA_DataType& t_type, const ::sgrn::ArenaTree& t_arena, UA_Variant& t_out);

} // namespace sgrn::gateway::adapters
