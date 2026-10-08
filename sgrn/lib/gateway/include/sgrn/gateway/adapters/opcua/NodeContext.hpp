#pragma once

#include <sgrn/gateway/adapters/opcua/tag_access.hpp>
#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <open62541/types.h>

namespace sgrn::wrappers::opcua
{
class TypeRegistry;
} // namespace sgrn::wrappers::opcua

namespace sgrn::gateway::twin
{
class PlcMemory;
struct PlcNode;
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
    /// Resolved twin symbol for this field (twin::PlcState lookup), cached to
    /// keep findSymbol()'s per-call string build + case-insensitive hash out
    /// of every write / aggregate read / delta-push callback. Populated
    /// eagerly at registration when the twin tree is already loaded;
    /// resolveSymbol() lazily resolves (once) otherwise.
    /// NOTE: assumes the registry is loaded before adapter start and not
    /// swapped at runtime (current gateway.cpp wiring). If schema hot-reload
    /// while serving is ever added, the cache must be invalidated (reset
    /// plc_node to nullptr on loadRegistry/clear).
    mutable const twin::PlcNode* plc_node{nullptr};
    /// Non-null when the field is projected as an OPC UA Enumeration. The
    /// pointed-to `UA_DataType` carries the node id used in the address space;
    /// `enum_map` mirrors it for value<->name translation on read/write.
    std::optional<double> min_val{std::nullopt};
    std::optional<double> max_val{std::nullopt};
    const UA_DataType* enum_type{nullptr};
    std::map<int, std::string> enum_map;
    const twin::PlcNode* resolveSymbol() const;

    /// Discrete-tag projection: when true this node is backed by tag_backing
    /// (not the twin). Scalar tags — and scalar members of UDT tags — decode
    /// through the shared scalar path; UDT tags project as folders with one
    /// node per member. tag_byte_offset addresses tag-relative bytes.
    bool is_tag{false};
    std::string tag_name;
    uint32_t tag_byte_offset{0};
    int tag_bit_index{-1};
    std::shared_ptr<const TagBacking> tag_backing;
};

} // namespace sgrn::gateway::adapters
