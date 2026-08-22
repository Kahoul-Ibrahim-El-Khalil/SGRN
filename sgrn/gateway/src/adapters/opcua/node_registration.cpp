#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/information_model.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>

#include <fmt/core.h>
#include <open62541/common.h>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;

namespace sgrn::gateway::adapters
{

extern UA_StatusCode readS7Value(
    UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void*, UA_Boolean, const UA_NumericRange*, UA_DataValue*);
extern UA_StatusCode readAggregateValue(
    UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void*, UA_Boolean, const UA_NumericRange*, UA_DataValue*);
extern UA_StatusCode writeS7Value(
    UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void*, const UA_NumericRange*, const UA_DataValue*);

static void trackNode(uint16_t t_db_number, const std::string& t_full_path, const wrappers::opcua::NodeId& t_var_id,
    const NodeContext* tp_ctx, std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map, DeltaPushHandler* tp_delta_push) {
    if (!tp_node_id_map)
        return;
    std::string map_key = fmt::format("{}:{}", t_db_number, t_full_path);
    (*tp_node_id_map).insert_or_assign(map_key, wrappers::opcua::nodeIdFromRaw(t_var_id.get()));
    if (tp_delta_push)
        tp_delta_push->registerNode(map_key, t_var_id, tp_ctx);
}

static void setReadOnlyDataSource(UA_Server* tp_server, const UA_NodeId& t_var_id) {
    UA_DataSource ds{};
    ds.read = readAggregateValue;
    UA_Server_setVariableNode_dataSource(tp_server, t_var_id, ds);
}

static void setReadWriteDataSource(UA_Server* tp_server, const UA_NodeId& t_var_id) {
    UA_DataSource ds{};
    ds.read = readS7Value;
    ds.write = writeS7Value;
    UA_Server_setVariableNode_dataSource(tp_server, t_var_id, ds);
}

static NodeContext* makeFieldContext(twin::PlcMemory* tp_plc_memory, uint16_t t_db_number, const std::string& t_full_path,
    ::sgrn::gateway::SecurityManager* tp_security, const DbField& t_field, bool t_is_array, bool t_is_custom_udt, int t_ua_type_idx,
    uint32_t t_field_size, wrappers::opcua::TypeRegistry& t_type_registry, bool t_trigger_events)

{
    NodeContext* p_ctx = new NodeContext{
        .server = tp_plc_memory,
        .db_number = t_db_number,
        .field_path = t_full_path,
        .security = tp_security,
        .array_length = t_is_array ? static_cast<uint32_t>(t_field.count) : 0u,
        .elem_ua_type_index = t_ua_type_idx,
        .udt_name = (::sgrn::scl::kind_of(t_field) == ::sgrn::scl::FieldKind::Struct) ? t_field.udt_name : "",
        .type_registry = &t_type_registry,
        .trigger_events = t_trigger_events,
        .field_offset = static_cast<uint32_t>(t_field.offset),
        .field_size = t_field_size,
        .type = t_field.type,
        .kind = ::sgrn::scl::kind_of(t_field),
        .string_capacity = static_cast<uint32_t>(t_field.string_capacity),
        .scratch_buf = {},
        .enum_type = nullptr,
        .enum_map = {},
    };

    p_ctx->scratch_buf.resize(p_ctx->field_size);
    p_ctx->enum_map = t_field.enum_map;

    if (p_ctx->kind == ::sgrn::scl::FieldKind::Enum) {
        const int ua_base = dataTypeToUaTypeIndex(t_field.type).value();
        const std::string sig = enumTypeSignature(ua_base, t_field.enum_map);
        if (const UA_DataType* p_enum = t_type_registry.findEnumBySignature(sig))
            p_ctx->enum_type = p_enum;
    }
    return p_ctx;
}

static uint32_t computeFieldSize(const DbField& t_field, bool t_is_array) {
    if (t_field.type == DataType::Struct)
        return static_cast<uint32_t>(t_field.struct_size);

    if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
        t_field.type == DataType::XWString) {
        // struct_size is the per-element byte span after the offset-tracker fix.
        // Return the TOTAL field size (per-element span × element count) so that
        // readDbMemory reads the entire string array in one call.
        const uint32_t elem_span = static_cast<uint32_t>(
            t_field.struct_size > 0
                ? t_field.struct_size
                : s7codec::typeSpanBytes(t_field.type, t_field.string_capacity > 0 ? t_field.string_capacity : t_field.count));
        const uint32_t n_elems = static_cast<uint32_t>(t_field.count > 0 ? t_field.count : 1);
        return elem_span * n_elems;
    }

    return static_cast<uint32_t>(t_is_array ? s7codec::typeSpanBytes(t_field.type, t_field.count) : s7codec::primitiveSize(t_field.type));
}

static void configureVariableAttributes(UA_VariableAttributes& t_v_attr, const DbField& t_field, bool t_is_array, bool t_is_custom_udt,
    int t_ua_type_idx, const UA_DataType* tp_custom_type, const UA_DataType* tp_enum_type) {
    t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    t_v_attr.userAccessLevel = t_v_attr.accessLevel;

    if (::sgrn::scl::kind_of(t_field) == ::sgrn::scl::FieldKind::Enum && tp_enum_type) {
        // OPC UA Part 3 §5.6.2: Enumeration variable nodes MUST declare their
        // DataType as the specific Enum DataType NodeId so clients can discover
        // the symbolic names. However, open62541's pre-write type validation
        // checks if the incoming UA_Variant type is equal to or a *subtype* of
        // the declared DataType. Clients send enum values as UA_Int32 (wire
        // encoding per Part 6 §5.2.2.5). Since Int32 is an *ancestor* of our
        // enum (Int32 → Enumeration → Status), the check fails with
        // BadTypeMismatch. Fix: keep the custom enum NodeId as the DataType
        // attribute (visible in the address space for type discovery), but the
        // open62541 server internal type check is bypassed by setting the
        // attribute directly. We use tp_enum_type->typeId which IS the enum
        // NodeId — this is correct per spec. The BadTypeMismatch was caused
        // by open62541 checking subtype direction incorrectly for DataSource
        // nodes; we work around it by setting DataType = Int32 so the wire
        // type matches, while keeping enum discovery through the DataType tree.
        //
        // Per OPC UA Part 3 §8.14, an Enumeration subtype variable node's
        // DataType SHOULD be the Enum type, but INT32 is also legal and avoids
        // the open62541 limitation.
        t_v_attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_INT32);
        if (t_is_array) {
            t_v_attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
            t_v_attr.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            t_v_attr.arrayDimensions[0] = static_cast<UA_UInt32>(t_field.count);
            t_v_attr.arrayDimensionsSize = 1;
        } else {
            t_v_attr.valueRank = UA_VALUERANK_SCALAR;
        }
        return;
    }

    if (t_is_custom_udt && tp_custom_type) {
        t_v_attr.dataType = tp_custom_type->typeId;
        if (t_is_array) {
            t_v_attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
            t_v_attr.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
            t_v_attr.arrayDimensions[0] = static_cast<UA_UInt32>(t_field.count);
            t_v_attr.arrayDimensionsSize = 1;
        } else {
            t_v_attr.valueRank = UA_VALUERANK_SCALAR;
        }
    } else if (t_is_array && t_ua_type_idx >= 0) {
        t_v_attr.dataType = UA_TYPES[t_ua_type_idx].typeId;
        t_v_attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
        t_v_attr.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
        t_v_attr.arrayDimensions[0] = static_cast<UA_UInt32>(t_field.count);
        t_v_attr.arrayDimensionsSize = 1;
    } else {
        t_v_attr.valueRank = UA_VALUERANK_SCALAR;
        if (t_ua_type_idx >= 0)
            t_v_attr.dataType = UA_TYPES[t_ua_type_idx].typeId;
    }
}

static void addEngineeringUnitsProperty(
    UA_Server* tp_server, const UA_NodeId& t_parent_id, const std::string& t_full_node_id, const std::string& t_unit_str) {

    UA_NodeId prop_id = UA_NODEID_STRING_ALLOC(1, (t_full_node_id + ".EngineeringUnits").c_str());
    UA_VariableAttributes eu_attr = UA_VariableAttributes_default;
    eu_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "EngineeringUnits");
    eu_attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_EUINFORMATION);
    UA_EUInformation eu;
    UA_EUInformation_init(&eu);
    eu.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_unit_str.c_str());
    UA_Variant_setScalarCopy(&eu_attr.value, &eu, &UA_TYPES[UA_TYPES_EUINFORMATION]);
    UA_Server_addVariableNode(tp_server, prop_id, t_parent_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(0, "EngineeringUnits"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), eu_attr, nullptr, nullptr);
    UA_NodeId_clear(&prop_id);
    UA_EUInformation_clear(&eu);
}

void addAggregateValueNode(const OpcUaAdapterContext& t_adapter_ctx, const OpcUaNodeRegistryContext& t_nodes_ctx,
    const OpcUaNodePath& t_path, const OpcUaDbContext& t_db, const OpcUaAggregateFieldContext& t_aggregate) {
    UA_Server* p_raw = t_adapter_ctx.p_opcua_server->raw();
    const UA_NodeId& parent = t_path.parent_id.get();

    auto* p_ctx = new NodeContext{
        .server = t_adapter_ctx.p_plc_memory,
        .db_number = t_db.number,
        .field_path = t_path.path,
        .security = t_adapter_ctx.p_security_manager,
        .array_length = 0u,
        .elem_ua_type_index = -1,
        .udt_name = t_aggregate.udt_name,
        .type_registry = t_adapter_ctx.p_type_registry,
        .trigger_events = t_db.trigger_events,
        .field_offset = t_aggregate.field_offset,
        .field_size = t_aggregate.field_size,
        .type = ::sgrn::scl::DataType::Struct,
        .enum_type = nullptr,
        .enum_map = {},
    };
    t_nodes_ctx.p_owned_contexts->push_back(std::unique_ptr<NodeContext>(p_ctx));

    UA_NodeId t_var_id = UA_NODEID_STRING_ALLOC(1, (t_path.node_id + ".Value").c_str());
    UA_VariableAttributes t_v_attr = UA_VariableAttributes_default;
    t_v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "Value");

    if (!t_aggregate.udt_name.empty()) {
        const UA_DataType* p_udt_type = t_adapter_ctx.p_type_registry->find(t_aggregate.udt_name);
        if (p_udt_type) {
            t_v_attr.dataType = p_udt_type->typeId;
            t_v_attr.valueRank = UA_VALUERANK_SCALAR;
            t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
            t_v_attr.userAccessLevel = t_v_attr.accessLevel;
        } else {
            t_v_attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRING);
            t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
            t_v_attr.userAccessLevel = t_v_attr.accessLevel;
        }
    } else {
        t_v_attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRING);
        t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
        t_v_attr.userAccessLevel = t_v_attr.accessLevel;
    }

    UA_Server_addVariableNode(p_raw, t_var_id, parent, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), UA_QUALIFIEDNAME_ALLOC(1, "Value"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), t_v_attr, p_ctx, nullptr);

    setReadOnlyDataSource(p_raw, t_var_id);
    trackNode(t_db.number, t_path.path, wrappers::opcua::nodeIdFromRaw(t_var_id), p_ctx, t_nodes_ctx.p_node_id_map,
        t_adapter_ctx.p_delta_push_handler);
    UA_NodeId_clear(&t_var_id);
}
void addFieldNodes(const OpcUaAdapterContext& t_adapter_ctx, const OpcUaNodeRegistryContext& t_nodes,
    const wrappers::opcua::NodeId& t_parent_id, const std::vector<DbField>& t_fields, const std::string& t_path_prefix,
    const std::string& t_node_id_prefix, const OpcUaDbContext& t_db) {
    for (const DbField& t_field : t_fields) {
        const std::string t_full_path = t_path_prefix.empty() ? t_field.name : t_path_prefix + "." + t_field.name;

        const std::string t_full_node_id = t_node_id_prefix + "." + t_field.name;

        const bool t_is_custom_udt = !t_field.udt_name.empty() && t_adapter_ctx.p_type_registry->find(t_field.udt_name) != nullptr;

        const bool current_trigger_events = t_db.trigger_events || t_field.trigger_events;

        // After:
        OpcUaNodePath node_path{
            .parent_id = wrappers::opcua::nodeIdFromRaw(t_parent_id.get()),
            .path = t_full_path,
            .node_id = t_full_node_id,
        };
        OpcUaDbContext node_db{
            .number = t_db.number,
            .trigger_events = current_trigger_events,
        };

        if (t_is_custom_udt || t_field.children.empty()) {
            addLeafVariableNode(t_adapter_ctx, t_nodes, node_path, t_field, node_db);
        } else {
            addFolderNode(t_adapter_ctx, t_nodes, node_path, t_field, node_db);
        }
    }
}
void addLeafVariableNode(const OpcUaAdapterContext& t_adapter_ctx, const OpcUaNodeRegistryContext& t_nodes_ctx, const OpcUaNodePath& t_path,
    const ::sgrn::scl::DbField& t_field, const OpcUaDbContext& t_db) {
    UA_Server* p_raw = t_adapter_ctx.p_opcua_server->raw();
    const UA_NodeId& parent = t_path.parent_id.get();

    const bool t_is_array = (t_field.count > 1);
    const bool t_is_custom_udt = !t_field.udt_name.empty() && t_adapter_ctx.p_type_registry->find(t_field.udt_name) != nullptr;

    int t_ua_type_idx = -1;
    const UA_DataType* p_custom_type = nullptr;
    const UA_DataType* p_enum_type = nullptr;
    if (t_is_custom_udt) {
        p_custom_type = t_adapter_ctx.p_type_registry->find(t_field.udt_name);
    } else {
        t_ua_type_idx = dataTypeToUaTypeIndex(t_field.type).value();
    }

    if (!t_field.enum_map.empty()) {
        const std::string sig = enumTypeSignature(t_ua_type_idx, t_field.enum_map);
        p_enum_type = t_adapter_ctx.p_type_registry->findEnumBySignature(sig);
    }

    const uint32_t t_field_size = computeFieldSize(t_field, t_is_array);
    auto* p_ctx = makeFieldContext(t_adapter_ctx.p_plc_memory, t_db.number, t_path.path, t_adapter_ctx.p_security_manager, t_field,
        t_is_array, t_is_custom_udt, t_ua_type_idx, t_field_size, *t_adapter_ctx.p_type_registry, t_db.trigger_events);
    p_ctx->enum_type = p_enum_type;
    t_nodes_ctx.p_owned_contexts->push_back(std::unique_ptr<NodeContext>(p_ctx));

    UA_NodeId t_var_id = UA_NODEID_STRING_ALLOC(1, t_path.node_id.c_str());
    UA_VariableAttributes t_v_attr = UA_VariableAttributes_default;
    t_v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_field.name.c_str());
    configureVariableAttributes(t_v_attr, t_field, t_is_array, t_is_custom_udt, t_ua_type_idx, p_custom_type, p_enum_type);

    UA_Server_addVariableNode(p_raw, t_var_id, parent, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME_ALLOC(1, t_field.name.c_str()), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), t_v_attr, p_ctx, nullptr);

    setReadWriteDataSource(p_raw, t_var_id);

    if (t_field.unit.has_value())
        addEngineeringUnitsProperty(p_raw, t_var_id, t_path.node_id, t_field.unit.value());

    wrappers::opcua::NodeId tracked = wrappers::opcua::nodeIdFromRaw(t_var_id);
    trackNode(t_db.number, t_path.path, tracked, p_ctx, t_nodes_ctx.p_node_id_map, t_adapter_ctx.p_delta_push_handler);

    if (!t_field.children.empty()) {
        OpcUaDbContext db_ctx{t_db.number, t_db.trigger_events};
        addFieldNodes(t_adapter_ctx, t_nodes_ctx, tracked, t_field.children, t_path.path, t_path.node_id, db_ctx);
    }

    UA_NodeId_clear(&t_var_id);
}
void addFolderNode(const OpcUaAdapterContext& t_adapter, const OpcUaNodeRegistryContext& t_nodes, const OpcUaNodePath& t_path,
    const ::sgrn::scl::DbField& t_field, const OpcUaDbContext& t_db) {
    UA_Server* p_raw = t_adapter.p_opcua_server->raw();
    const UA_NodeId& parent = t_path.parent_id.get();

    UA_NodeId branch_id = UA_NODEID_STRING_ALLOC(1, t_path.node_id.c_str());

    UA_ObjectAttributes b_attr = UA_ObjectAttributes_default;
    b_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_field.name.c_str());
    b_attr.eventNotifier = UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT;

    UA_Server_addObjectNode(p_raw, branch_id, parent, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME_ALLOC(1, t_field.name.c_str()), UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), b_attr, nullptr, nullptr);

    UA_ExpandedNodeId branch_exp_id;
    UA_ExpandedNodeId_init(&branch_exp_id);
    branch_exp_id.nodeId = branch_id;
    UA_Server_addReference(p_raw, parent, UA_NODEID_NUMERIC(0, 48 /* HasNotifier */), branch_exp_id, true);

    const uint32_t struct_size =
        t_field.type == DataType::Struct ? static_cast<uint32_t>(t_field.struct_size) : computeFieldSize(t_field, false);
    OpcUaAggregateFieldContext agg_ctx{t_field.udt_name, static_cast<uint32_t>(t_field.offset), struct_size};

    OpcUaNodePath agg_path{wrappers::opcua::nodeIdFromRaw(branch_id), t_path.path, t_path.node_id};
    addAggregateValueNode(t_adapter, t_nodes, agg_path, t_db, agg_ctx);

    // FIX: without this, unresolved/anonymous struct fields only ever exposed
    // the single opaque ".Value" JSON aggregate — clients never saw the
    // individual member fields as their own browsable/writable nodes.
    if (!t_field.children.empty()) {
        addFieldNodes(t_adapter, t_nodes, wrappers::opcua::nodeIdFromRaw(branch_id), t_field.children, t_path.path, t_path.node_id, t_db);
    }

    UA_NodeId_clear(&branch_id);
}
} // namespace sgrn::gateway::adapters
