#pragma once

#include <sgrn/gateway/core/LeafFieldMeta.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sgrn::gateway::twin
{

struct FieldUpdateNotification {
    uint16_t db{0};
    /// OPC UA / subscription relative path (without DB segment prefix).
    std::string path;
    /// Full PlcState lookup path (includes DB segment prefix).
    std::string state_path;
    uint64_t timestamp{0};
    std::string json_value;
    ::sgrn::gateway::core::TypedLeafPayload typed_leaf;
};

FieldUpdateNotification makeFieldUpdateNotification(PlcState& t_state, const PlcNode& t_node, const DbEntry& t_entry,
    const std::string& t_state_path, uint64_t t_timestamp, bool t_include_json = false);

std::vector<FieldUpdateNotification> gatherTypedDirtyLeaves(
    PlcState& t_state, const std::string& t_segment_name, bool t_include_json = false);

std::vector<FieldUpdateNotification> gatherTypedDirtyLeavesByDb(PlcState& t_state, uint16_t t_db_number, bool t_include_json = false);

} // namespace sgrn::gateway::twin
