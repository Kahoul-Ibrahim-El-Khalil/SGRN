#pragma once

#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/delta_push.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/types.hpp>

#include <open62541/server.h>

#include <map>
#include <memory>
#include <optional>
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

// ── Adapter context ─────────────────────────────────────────────────────────
//
// Long-lived services shared by OPC UA adapter operations.

struct OpcUaAdapterContext {
    wrappers::opcua::Server* p_opcua_server;
    twin::PlcMemory* p_plc_memory;
    ::sgrn::gateway::SecurityManager* p_security_manager;
    wrappers::opcua::TypeRegistry* p_type_registry;
    DeltaPushHandler* p_delta_push_handler;
};

// ── Node registry context ────────────────────────────────────────────────────
//
// Mutable state produced while OPC UA nodes are registered.

struct OpcUaNodeRegistryContext {
    std::vector<std::unique_ptr<NodeContext>>* p_owned_contexts;
    std::unordered_map<std::string, wrappers::opcua::NodeId>* p_node_id_map;
};

// ── Node path ────────────────────────────────────────────────────────────────
//
// Identifies the location of a node in the OPC UA address space.

struct OpcUaNodePath {
    wrappers::opcua::NodeId parent_id;
    std::string path;
    std::string node_id;
};

// ── DB context ───────────────────────────────────────────────────────────────
//
// Information inherited by nodes belonging to a PLC data block.

struct OpcUaDbContext {
    uint16_t number;
    bool trigger_events;
};

// ── Aggregate field context ──────────────────────────────────────────────────
//
// Additional metadata required when an OPC UA node represents an aggregate
// value or a field inside a UDT.

struct OpcUaAggregateFieldContext {
    std::string udt_name;
    uint32_t field_offset = 0;
    uint32_t field_size = 0;
};

// ── Address-space context ────────────────────────────────────────────────────
//
// Complete state required while constructing the OPC UA address space.

struct OpcUaAddressSpaceContext {
    OpcUaAdapterContext adapter;
    OpcUaNodeRegistryContext nodes;
    wrappers::opcua::NodeId* p_alarm_event_type_id;
};

// ── Node registration ───────────────────────────────────────────────────────

void addFieldNodes(const OpcUaAdapterContext& t_adapter, const OpcUaNodeRegistryContext& t_nodes,
    const wrappers::opcua::NodeId& t_parent_id, const std::vector<::sgrn::scl::DbField>& t_fields, const std::string& t_path_prefix,
    const std::string& t_node_id_prefix, const OpcUaDbContext& t_db);

void addAggregateValueNode(const OpcUaAdapterContext& t_adapter_ctx, const OpcUaNodeRegistryContext& t_nodes_ctx,
    const OpcUaNodePath& t_path, const OpcUaDbContext& t_db, const OpcUaAggregateFieldContext& t_aggregate = {});

void addLeafVariableNode(const OpcUaAdapterContext& t_adapter_ctx, const OpcUaNodeRegistryContext& t_nodes_ctx, const OpcUaNodePath& t_path,
    const ::sgrn::scl::DbField& t_field, const OpcUaDbContext& t_db);

void addFolderNode(const OpcUaAdapterContext& t_adapter, const OpcUaNodeRegistryContext& t_nodes_ctx, const OpcUaNodePath& t_path,
    const ::sgrn::scl::DbField& t_field,
    const OpcUaDbContext& t_db); // ── Information model ───────────────────────────────────────────────────────

void registerEnumDataType(UA_Server* tp_raw, const UA_DataType& t_type, const sgrn::gateway::wrappers::opcua::EnumTypeDef& t_def);

void registerDataTypeNodes(wrappers::opcua::Server& t_server, const wrappers::opcua::TypeRegistry& t_type_registry);

void registerEventTypes(wrappers::opcua::Server& t_server, wrappers::opcua::NodeId& t_alarm_event_type_id);

void buildAddressSpace(const OpcUaAddressSpaceContext& t_context, const ::sgrn::scl::PlcSchemaStore& t_registry);

// ── Alarm events ────────────────────────────────────────────────────────────

void triggerAlarmEvent(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_alarm_event_type_id, uint16_t t_db_number,
    const std::string& t_path, const rapidjson::Value& t_alarm_obj, uint64_t t_timestamp_ms);

} // namespace sgrn::gateway::adapters
