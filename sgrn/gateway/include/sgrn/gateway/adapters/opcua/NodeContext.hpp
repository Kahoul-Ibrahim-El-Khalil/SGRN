#pragma once

#include <sgrn/scl/types.hpp>
#include <string>
#include <unordered_map>

#include <open62541/types.h>

namespace sgrn::gateway::wrappers::opcua
{
class TypeRegistry;
} // namespace sgrn::gateway::wrappers::opcua

namespace sgrn::gateway::twin
{
class PlcMemory;
} // namespace sgrn::gateway::twin

namespace sgrn::gateway
{
class SecurityManager;
} // namespace sgrn::gateway

namespace sgrn::gateway::adapters
{

struct NodeContext {
    twin::PlcMemory* server{nullptr};
    uint16_t db_number{0};
    std::string field_path;
    ::sgrn::gateway::SecurityManager* security{nullptr};
    uint32_t array_length{0};   // field.count when > 1 and not string, else 0
    int elem_ua_type_index{-1}; // UA_TYPES_* for array elements, -1 = JSON-string fallback
    std::string udt_name;
    wrappers::opcua::TypeRegistry* type_registry{nullptr};
    bool trigger_events{false};
    uint32_t field_offset{0};
    uint32_t field_size{0};
    ::sgrn::scl::DataType type{::sgrn::scl::DataType::Byte};
    ::sgrn::scl::FieldKind kind{::sgrn::scl::FieldKind::Scalar};

    uint32_t string_capacity;
    mutable std::vector<uint8_t> scratch_buf; // sized once at registration, reused every read for decoded values;
    /// Non-null when the field is projected as an OPC UA Enumeration. The
    /// pointed-to `UA_DataType` carries the node id used in the address space;
    /// `enum_map` mirrors it for value<->name translation on read/write.
    const UA_DataType* enum_type{nullptr};
    std::map<int, std::string> enum_map;
};

} // namespace sgrn::gateway::adapters
