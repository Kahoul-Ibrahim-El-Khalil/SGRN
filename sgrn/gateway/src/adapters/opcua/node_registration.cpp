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

static NodeContext* makeFieldContext(twin::PlcMemory* tp_s7_server, uint16_t t_db_number, const std::string& t_full_path,
    ::sgrn::gateway::SecurityManager* tp_security, const DbField& t_field, bool t_is_array, bool t_is_custom_udt, int t_ua_type_idx,
    uint32_t t_field_size, wrappers::opcua::TypeRegistry& t_type_registry, bool t_trigger_events) {
    return new NodeContext{tp_s7_server, t_db_number, t_full_path, tp_security, t_is_array ? static_cast<uint32_t>(t_field.count) : 0u,
        t_ua_type_idx, t_is_custom_udt ? t_field.udt_name : "", &t_type_registry, t_trigger_events, static_cast<uint32_t>(t_field.offset),
        t_field_size, t_field.type};
}

static uint32_t computeFieldSize(const DbField& t_field, bool t_is_array) {
    if (t_field.type == DataType::Struct)
        return static_cast<uint32_t>(t_field.struct_size);

    if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
        t_field.type == DataType::XWString) {
        int max_len = (t_field.struct_size > 0 ? t_field.struct_size : t_field.count);
        return static_cast<uint32_t>(s7codec::typeSpanBytes(t_field.type, max_len));
    }

    return static_cast<uint32_t>(t_is_array ? s7codec::typeSpanBytes(t_field.type, t_field.count) : s7codec::primitiveSize(t_field.type));
}

static void configureVariableAttributes(UA_VariableAttributes& t_v_attr, const DbField& t_field, bool t_is_array, bool t_is_custom_udt,
    int t_ua_type_idx, const UA_DataType* tp_custom_type) {
    t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

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

void addAggregateValueNode(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const std::string& t_path_prefix,
    const std::string& t_node_id_suffix, uint16_t t_db_number, twin::PlcMemory* tp_s7_server, ::sgrn::gateway::SecurityManager* tp_security,
    std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts, std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map,
    wrappers::opcua::TypeRegistry& t_type_registry, bool t_trigger_events, DeltaPushHandler* tp_delta_push, const std::string& t_udt_name,
    uint32_t t_field_offset, uint32_t t_field_size) {
    UA_Server* p_raw = t_server.raw();
    const UA_NodeId& parent = t_parent_id.get();

    auto* p_ctx = new NodeContext{tp_s7_server, t_db_number, t_path_prefix, tp_security, 0u, -1, t_udt_name, &t_type_registry,
        t_trigger_events, t_field_offset, t_field_size, DataType::Struct};
    t_owned_contexts.push_back(std::unique_ptr<NodeContext>(p_ctx));

    UA_NodeId t_var_id = UA_NODEID_STRING_ALLOC(1, (t_node_id_suffix + ".Value").c_str());
    UA_VariableAttributes t_v_attr = UA_VariableAttributes_default;
    t_v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "Value");

    if (!t_udt_name.empty()) {
        const UA_DataType* p_udt_type = t_type_registry.find(t_udt_name);
        if (p_udt_type) {
            t_v_attr.dataType = p_udt_type->typeId;
            t_v_attr.valueRank = UA_VALUERANK_SCALAR;
            t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
        } else {
            t_v_attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRING);
            t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
        }
    } else {
        t_v_attr.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRING);
        t_v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    }

    UA_Server_addVariableNode(p_raw, t_var_id, parent, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), UA_QUALIFIEDNAME_ALLOC(1, "Value"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), t_v_attr, p_ctx, nullptr);

    setReadOnlyDataSource(p_raw, t_var_id);
    trackNode(t_db_number, t_path_prefix, wrappers::opcua::nodeIdFromRaw(t_var_id), p_ctx, tp_node_id_map, tp_delta_push);
    UA_NodeId_clear(&t_var_id);
}

void addFieldNodes(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const std::vector<DbField>& t_fields,
    const std::string& t_path_prefix, const std::string& t_node_id_prefix, uint16_t t_db_number, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security, std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    bool t_parent_trigger_events, DeltaPushHandler* tp_delta_push) {
    for (const DbField& t_field : t_fields) {
        const std::string t_full_path = t_path_prefix.empty() ? t_field.name : t_path_prefix + "." + t_field.name;
        const std::string t_full_node_id = t_node_id_prefix + "." + t_field.name;
        const bool t_is_custom_udt = !t_field.udt_name.empty() && t_type_registry.find(t_field.udt_name) != nullptr;
        const bool current_trigger_events = t_parent_trigger_events || t_field.trigger_events;

        if (t_is_custom_udt || t_field.children.empty()) {
            addLeafVariableNode(t_server, t_parent_id, t_field, t_full_path, t_full_node_id, t_db_number, tp_s7_server, tp_security,
                t_owned_contexts, tp_node_id_map, t_type_registry, current_trigger_events, tp_delta_push);
        } else {
            addFolderNode(t_server, t_parent_id, t_field, t_full_path, t_full_node_id, t_db_number, tp_s7_server, tp_security,
                t_owned_contexts, tp_node_id_map, t_type_registry, current_trigger_events, tp_delta_push);
        }
    }
}

void addLeafVariableNode(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const DbField& t_field,
    const std::string& t_full_path, const std::string& t_full_node_id, uint16_t t_db_number, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security, std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    bool t_trigger_events, DeltaPushHandler* tp_delta_push) {
    UA_Server* p_raw = t_server.raw();
    const UA_NodeId& parent = t_parent_id.get();

    const bool is_string_type = (t_field.type == DataType::String || t_field.type == DataType::WString ||
                                 t_field.type == DataType::XString || t_field.type == DataType::XWString);
    const bool t_is_array = (t_field.count > 1) && (!is_string_type || t_field.struct_size > 0);
    const bool t_is_custom_udt = !t_field.udt_name.empty() && t_type_registry.find(t_field.udt_name) != nullptr;

    int t_ua_type_idx = -1;
    const UA_DataType* p_custom_type = nullptr;
    if (t_is_custom_udt)
        p_custom_type = t_type_registry.find(t_field.udt_name);
    else
        t_ua_type_idx = s7TypeToUaTypeIndex(t_field.type);

    const uint32_t t_field_size = computeFieldSize(t_field, t_is_array);
    auto* p_ctx = makeFieldContext(tp_s7_server, t_db_number, t_full_path, tp_security, t_field, t_is_array, t_is_custom_udt, t_ua_type_idx,
        t_field_size, t_type_registry, t_trigger_events);
    t_owned_contexts.push_back(std::unique_ptr<NodeContext>(p_ctx));

    UA_NodeId t_var_id = UA_NODEID_STRING_ALLOC(1, t_full_node_id.c_str());
    UA_VariableAttributes t_v_attr = UA_VariableAttributes_default;
    t_v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_field.name.c_str());
    configureVariableAttributes(t_v_attr, t_field, t_is_array, t_is_custom_udt, t_ua_type_idx, p_custom_type);

    UA_Server_addVariableNode(p_raw, t_var_id, parent, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME_ALLOC(1, t_field.name.c_str()), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), t_v_attr, p_ctx, nullptr);

    setReadWriteDataSource(p_raw, t_var_id);

    if (t_field.unit.has_value())
        addEngineeringUnitsProperty(p_raw, t_var_id, t_full_node_id, t_field.unit.value());

    wrappers::opcua::NodeId tracked = wrappers::opcua::nodeIdFromRaw(t_var_id);
    trackNode(t_db_number, t_full_path, tracked, p_ctx, tp_node_id_map, tp_delta_push);

    if (!t_field.children.empty()) {
        addFieldNodes(t_server, tracked, t_field.children, t_full_path, t_full_node_id, t_db_number, tp_s7_server, tp_security,
            t_owned_contexts, tp_node_id_map, t_type_registry, t_trigger_events, tp_delta_push);
    }

    UA_NodeId_clear(&t_var_id);
}

void addFolderNode(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_parent_id, const DbField& t_field,
    const std::string& t_full_path, const std::string& t_full_node_id, uint16_t t_db_number, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security, std::vector<std::unique_ptr<NodeContext>>& t_owned_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>* tp_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    bool t_trigger_events, DeltaPushHandler* tp_delta_push) {
    UA_Server* p_raw = t_server.raw();
    const UA_NodeId& parent = t_parent_id.get();

    UA_NodeId branch_id = UA_NODEID_STRING_ALLOC(1, t_full_node_id.c_str());

    UA_ObjectAttributes b_attr = UA_ObjectAttributes_default;
    b_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_field.name.c_str());
    b_attr.eventNotifier = UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT;

    UA_Server_addObjectNode(p_raw, branch_id, parent, UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME_ALLOC(1, t_field.name.c_str()), UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), b_attr, nullptr, nullptr);

    UA_ExpandedNodeId branch_exp_id;
    UA_ExpandedNodeId_init(&branch_exp_id);
    branch_exp_id.nodeId = branch_id;
    UA_Server_addReference(p_raw, parent, UA_NODEID_NUMERIC(0, 48 /* HasNotifier */), branch_exp_id, true);

    wrappers::opcua::NodeId branch_node = wrappers::opcua::nodeIdFromRaw(branch_id);
    const uint32_t struct_size =
        t_field.type == DataType::Struct ? static_cast<uint32_t>(t_field.struct_size) : computeFieldSize(t_field, false);
    addAggregateValueNode(t_server, branch_node, t_full_path, t_full_node_id, t_db_number, tp_s7_server, tp_security, t_owned_contexts,
        tp_node_id_map, t_type_registry, t_trigger_events, tp_delta_push, t_field.udt_name, static_cast<uint32_t>(t_field.offset),
        struct_size);

    addFieldNodes(t_server, branch_node, t_field.children, t_full_path, t_full_node_id, t_db_number, tp_s7_server, tp_security,
        t_owned_contexts, tp_node_id_map, t_type_registry, t_trigger_events, tp_delta_push);

    UA_NodeId_clear(&branch_id);
}

} // namespace sgrn::gateway::adapters
