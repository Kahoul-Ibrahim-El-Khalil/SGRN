#pragma once

#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/delta_push.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>
#include <sgrn/scl/types.hpp>
#include <rapidjson/document.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::wrappers::opcua
{
class Server;
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

namespace sgrn::scl
{
class PlcSchemaStore;
} // namespace sgrn::scl

namespace sgrn::gateway::adapters
{

// ── Node registration ───────────────────────────────────────────────────────

void addFieldNodes(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id,
    const std::vector<::sgrn::scl::DbField>& t_fields, const std::string& t_path_prefix, const std::string& t_node_id_prefix,
    uint16_t t_db_number, twin::PlcMemory* tp_s7_server, ::sgrn::gateway::SecurityManager* tp_security,
    std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts, std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map,
    wrappers::opcua::TypeRegistry& t_type_registry, bool t_parent_trigger_events, DeltaPushHandler* tp_delta_push);

void addAggregateValueNode(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const std::string& t_path_prefix,
    const std::string& t_node_id_suffix, uint16_t t_db_number, twin::PlcMemory* tp_s7_server, ::sgrn::gateway::SecurityManager* tp_security,
    std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts, std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map,
    wrappers::opcua::TypeRegistry& t_type_registry, bool t_trigger_events, DeltaPushHandler* tp_delta_push,
    const std::string& t_udt_name = "", uint32_t t_field_offset = 0, uint32_t t_field_size = 0);

void addLeafVariableNode(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const ::sgrn::scl::DbField& t_field,
    const std::string& t_full_path, const std::string& t_full_node_id, uint16_t t_db_number, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security, std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    bool t_trigger_events, DeltaPushHandler* tp_delta_push);

void addFolderNode(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const ::sgrn::scl::DbField& t_field,
    const std::string& t_full_path, const std::string& t_full_node_id, uint16_t t_db_number, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security, std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    bool t_trigger_events, DeltaPushHandler* tp_delta_push);

// ── Information model ───────────────────────────────────────────────────────

void registerDataTypeNodes(wrappers::opcua::Server& t_server, const wrappers::opcua::TypeRegistry& t_type_registry);

void registerEventTypes(wrappers::opcua::Server& t_server, wrappers::opcua::NodeId& t_alarm_event_type_id);

void buildAddressSpace(wrappers::opcua::Server& t_server, const ::sgrn::scl::PlcSchemaStore& t_registry, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security, std::vector<std::unique_ptr<NodeContext>>& t_node_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>& t_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    wrappers::opcua::NodeId& t_alarm_event_type_id, DeltaPushHandler* tp_delta_push);

// ── Alarm events ────────────────────────────────────────────────────────────

void triggerAlarmEvent(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_alarm_event_type_id, uint16_t t_db_number,
    const std::string& t_path, const rapidjson::Value& t_alarm_obj, uint64_t t_timestamp_ms);

} // namespace sgrn::gateway::adapters
