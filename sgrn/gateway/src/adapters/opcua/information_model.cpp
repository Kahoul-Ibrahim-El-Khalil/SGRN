#include <fmt/core.h>

#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/information_model.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <functional>
#include <open62541/common.h>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

using sgrn::gateway::adapters::DeltaPushHandler;
using sgrn::gateway::adapters::NodeContext;
using sgrn::gateway::wrappers::opcua::EnumTypeDef;
using sgrn::gateway::wrappers::opcua::NodeId;
using sgrn::gateway::wrappers::opcua::nodeIdFromRaw;
using sgrn::gateway::wrappers::opcua::Server;
using sgrn::gateway::wrappers::opcua::TypeRegistry;
using ::sgrn::scl::DbField;

namespace sgrn::gateway::adapters
{

// ── Information model ───────────────────────────────────────────────────────

void registerEnumDataType(UA_Server* tp_raw, const UA_DataType& t_type, const EnumTypeDef& t_def) {
    UA_DataTypeAttributes attr = UA_DataTypeAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_type.typeName);

    UA_Server_addDataTypeNode(tp_raw, t_type.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_ENUMERATION), UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
        UA_QUALIFIEDNAME_ALLOC(1, t_type.typeName), attr, nullptr, nullptr);

    UA_EnumDefinition enum_def;
    UA_EnumDefinition_init(&enum_def);
    enum_def.fieldsSize = t_def.values.size();
    enum_def.fields = static_cast<UA_EnumField*>(UA_Array_new(enum_def.fieldsSize, &UA_TYPES[UA_TYPES_ENUMFIELD]));
    if (enum_def.fields) {
        size_t idx = 0;
        for (const auto& [k, v] : t_def.values) {
            UA_EnumField_init(&enum_def.fields[idx]);
            enum_def.fields[idx].value = static_cast<UA_Int64>(k);
            enum_def.fields[idx].displayName = UA_LOCALIZEDTEXT_ALLOC("en", v.c_str());
            enum_def.fields[idx].description = UA_LOCALIZEDTEXT_ALLOC("en", v.c_str());
            enum_def.fields[idx].name = UA_STRING_ALLOC(v.c_str());
            ++idx;
        }
        __UA_Server_write(tp_raw, &t_type.typeId, UA_ATTRIBUTEID_DATATYPEDEFINITION, &UA_TYPES[UA_TYPES_ENUMDEFINITION], &enum_def);
    }
    UA_EnumDefinition_clear(&enum_def);

    if (t_def.values.empty())
        return;

    // EnumStrings: indexed LocalizedText array.
    UA_NodeId strings_id = UA_NODEID_STRING_ALLOC(1, (std::string(t_type.typeName) + "_EnumStrings").c_str());

    const size_t n = t_def.values.size();

    UA_LocalizedText* lts = static_cast<UA_LocalizedText*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]));

    size_t idx = 0;

    for (const auto& [k, v] : t_def.values)
        lts[idx++] = UA_LOCALIZEDTEXT_ALLOC("en", v.c_str());

    UA_Variant strings_var;
    UA_Variant_init(&strings_var);

    UA_Variant_setArray(&strings_var, lts, static_cast<UA_Int32>(n), &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);

    UA_VariableAttributes s_attr = UA_VariableAttributes_default;

    s_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "EnumStrings");

    s_attr.dataType = UA_TYPES[UA_TYPES_LOCALIZEDTEXT].typeId;

    s_attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
    s_attr.arrayDimensionsSize = 1;

    UA_UInt32 dims[1] = {0};
    s_attr.arrayDimensions = dims;

    UA_Variant_copy(&strings_var, &s_attr.value);

    UA_Server_addVariableNode(tp_raw, strings_id, t_type.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(0, "EnumStrings"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), s_attr, nullptr, nullptr);

    UA_NodeId_clear(&strings_id);
    UA_Variant_clear(&strings_var);

#if defined(UA_TYPES_ENUMVALUETYPE)

    // EnumValues (value/displayName/description) — supported by
    // open62541 v1.x.
    UA_NodeId values_id = UA_NODEID_STRING_ALLOC(1, (std::string(t_type.typeName) + "_EnumValues").c_str());

    UA_EnumValueType* evs = static_cast<UA_EnumValueType*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_ENUMVALUETYPE]));

    idx = 0;

    for (const auto& [k, v] : t_def.values) {
        UA_EnumValueType_init(&evs[idx]);
        evs[idx].value = static_cast<UA_Int64>(k);
        evs[idx].displayName = UA_LOCALIZEDTEXT_ALLOC("en", v.c_str());
        ++idx;
    }

    UA_Variant values_var;
    UA_Variant_init(&values_var);

    UA_Variant_setArray(&values_var, evs, static_cast<UA_Int32>(n), &UA_TYPES[UA_TYPES_ENUMVALUETYPE]);

    UA_VariableAttributes v_attr = UA_VariableAttributes_default;

    v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "EnumValues");

    v_attr.dataType = UA_TYPES[UA_TYPES_ENUMVALUETYPE].typeId;

    v_attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
    v_attr.arrayDimensionsSize = 1;

    UA_UInt32 vdims[1] = {0};
    v_attr.arrayDimensions = vdims;

    UA_Variant_copy(&values_var, &v_attr.value);

    UA_Server_addVariableNode(tp_raw, values_id, t_type.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(0, "EnumValues"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), v_attr, nullptr, nullptr);

    UA_NodeId_clear(&values_id);
    UA_Variant_clear(&values_var);

#endif
}

void registerEventTypes(Server& t_server, NodeId& t_alarm_event_type_id) {
    UA_Server* p_raw = t_server.raw();

    UA_ObjectTypeAttributes attr = UA_ObjectTypeAttributes_default;

    attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "S7AlarmEventType");

    attr.description = UA_LOCALIZEDTEXT_ALLOC("en-US", "Event triggered when an S7 alarm is activated");

    UA_NodeId alarm_type_id = UA_NODEID_NUMERIC(1, 5000);

    UA_Server_addObjectTypeNode(p_raw, alarm_type_id, UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE), UA_QUALIFIEDNAME_ALLOC(1, "S7AlarmEventType"), attr, nullptr, nullptr);

    UA_VariableAttributes v_attr = UA_VariableAttributes_default;

    v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;

    UA_NodeId code_id = UA_NODEID_NUMERIC(1, 5001);

    v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "AlarmCode");

    v_attr.dataType = UA_TYPES[UA_TYPES_UINT16].typeId;

    UA_Server_addVariableNode(p_raw, code_id, alarm_type_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(1, "AlarmCode"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), v_attr, nullptr, nullptr);

    UA_NodeId prio_id = UA_NODEID_NUMERIC(1, 5002);

    v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "Priority");

    v_attr.dataType = UA_TYPES[UA_TYPES_BYTE].typeId;

    UA_Server_addVariableNode(p_raw, prio_id, alarm_type_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(1, "Priority"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), v_attr, nullptr, nullptr);

    t_alarm_event_type_id = nodeIdFromRaw(alarm_type_id);
}

void registerDataTypeNodes(Server& t_server, const TypeRegistry& t_type_registry) {
    UA_Server* p_raw = t_server.raw();

    for (const UA_DataType& ut : t_type_registry.types()) {

        if (ut.typeKind == UA_DATATYPEKIND_ENUM) {

            const EnumTypeDef* p_def = nullptr;

            for (const auto& def : t_type_registry.enumDefinitions()) {

                if (def.name == ut.typeName) {
                    p_def = &def;
                    break;
                }
            }

            if (!p_def)
                continue;

            registerEnumDataType(p_raw, ut, *p_def);

            continue;
        }

        UA_DataTypeAttributes attr = UA_DataTypeAttributes_default;

        attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", ut.typeName);

        UA_StructureDefinition sd;
        UA_StructureDefinition_init(&sd);

        sd.defaultEncodingId = ut.binaryEncodingId;
        sd.baseDataType = UA_NODEID_NUMERIC(0, UA_NS0ID_STRUCTURE);

        sd.structureType = UA_STRUCTURETYPE_STRUCTURE;

        sd.fieldsSize = ut.membersSize;

        sd.fields = static_cast<UA_StructureField*>(UA_Array_new(sd.fieldsSize, &UA_TYPES[UA_TYPES_STRUCTUREFIELD]));

        for (size_t j = 0; j < ut.membersSize; ++j) {

            UA_StructureField_init(&sd.fields[j]);

#ifdef UA_ENABLE_TYPEDESCRIPTION
            sd.fields[j].name = UA_STRING_ALLOC(ut.members[j].memberName);
#else
            sd.fields[j].name = UA_STRING_ALLOC("Member");
#endif

            UA_NodeId_copy(&ut.members[j].memberType->typeId, &sd.fields[j].dataType);

            sd.fields[j].valueRank = ut.members[j].isArray ? UA_VALUERANK_ONE_DIMENSION : UA_VALUERANK_SCALAR;
        }

        UA_StatusCode res = UA_Server_addDataTypeNode(p_raw, ut.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_STRUCTURE),
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE), UA_QUALIFIEDNAME_ALLOC(1, ut.typeName), attr, nullptr, nullptr);

        if (res == UA_STATUSCODE_GOOD) {
            __UA_Server_write(p_raw, &ut.typeId, UA_ATTRIBUTEID_DATATYPEDEFINITION, &UA_TYPES[UA_TYPES_STRUCTUREDEFINITION], &sd);
        }

        UA_ObjectAttributes enc_attr = UA_ObjectAttributes_default;

        enc_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "Default Binary");

        UA_Server_addObjectNode(p_raw, ut.binaryEncodingId, ut.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASENCODING),
            UA_QUALIFIEDNAME_ALLOC(0, "Default Binary"), UA_NODEID_NUMERIC(0, UA_NS0ID_DATATYPEENCODINGTYPE), enc_attr, nullptr, nullptr);

        UA_Array_delete(sd.fields, sd.fieldsSize, &UA_TYPES[UA_TYPES_STRUCTUREFIELD]);
    }
}

void buildAddressSpace(const OpcUaAddressSpaceContext& t_context, const ::sgrn::scl::PlcSchemaStore& t_registry) {
    const OpcUaAdapterContext& adapter = t_context.adapter;

    const OpcUaNodeRegistryContext& nodes = t_context.nodes;

    Server& server = *adapter.p_opcua_server;

    TypeRegistry& type_registry = *adapter.p_type_registry;

    NodeId& alarm_event_type_id = *t_context.p_alarm_event_type_id;

    registerDataTypeNodes(server, type_registry);

    registerEventTypes(server, alarm_event_type_id);

    UA_Server* p_raw = server.raw();

    UA_Byte event_notifier = UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT;

    UA_Server_writeEventNotifier(p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), event_notifier);

    UA_Server_writeEventNotifier(p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), event_notifier);

    UA_Server_addReference(p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), UA_NODEID_NUMERIC(0, 48 /* HasNotifier */),
        UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), true);

    for (const auto& [num, db] : t_registry.dbs()) {

        const std::string db_id_str = db.db_name.empty() ? fmt::format("DB{}", db.db_number) : db.db_name;

        UA_NodeId db_id = UA_NODEID_STRING_ALLOC(1, db_id_str.c_str());

        UA_ObjectAttributes db_attr = UA_ObjectAttributes_default;

        db_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", db_id_str.c_str());

        db_attr.eventNotifier = UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT;

        UA_Server_addObjectNode(p_raw, db_id, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            UA_QUALIFIEDNAME_ALLOC(1, db_id_str.c_str()), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), db_attr, nullptr, nullptr);

        UA_ExpandedNodeId db_exp_id;
        UA_ExpandedNodeId_init(&db_exp_id);

        db_exp_id.nodeId = db_id;

        UA_Server_addReference(
            p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, 48 /* HasNotifier */), db_exp_id, true);

        UA_NodeId prop_id = UA_NODEID_STRING_ALLOC(1, (db_id_str + ".DBNumber").c_str());

        UA_VariableAttributes p_attr = UA_VariableAttributes_default;

        p_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "DBNumber");

        const UA_UInt16 value = db.db_number;

        UA_Variant_setScalarCopy(&p_attr.value, &value, &UA_TYPES[UA_TYPES_UINT16]);

        UA_Server_addVariableNode(p_raw, prop_id, db_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), UA_QUALIFIEDNAME_ALLOC(0, "DBNumber"),
            UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), p_attr, nullptr, nullptr);

        UA_NodeId_clear(&prop_id);

        NodeId db_node = nodeIdFromRaw(db_id);

        OpcUaDbContext db_context{.number = num, .trigger_events = db.trigger_events};

        addFieldNodes(adapter, nodes, db_node, db.fields, "", db_id_str, db_context);

        UA_NodeId_clear(&db_id);
    }
}

void triggerAlarmEvent(Server& t_server, const NodeId& t_alarm_event_type_id, uint16_t t_db_number, const std::string& t_path,
    const rapidjson::Value& t_alarm_obj, uint64_t t_timestamp_ms) {
    if (!t_alarm_obj.IsObject())
        return;

    if (!t_alarm_obj.HasMember("active") || !t_alarm_obj["active"].IsBool() || !t_alarm_obj["active"].GetBool()) {
        return;
    }

    UA_Server* p_raw = t_server.raw();

    UA_NodeId event_node_id;

    UA_StatusCode res = UA_Server_createEvent(p_raw, t_alarm_event_type_id.get(), &event_node_id);

    if (res != UA_STATUSCODE_GOOD)
        return;

    UA_DateTime event_time = UA_DateTime_fromUnixTime(static_cast<UA_Int64>(t_timestamp_ms / 1000)) +
                             static_cast<UA_DateTime>((t_timestamp_ms % 1000) * UA_DATETIME_MSEC);

    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, (char*)"Time"), &event_time, &UA_TYPES[UA_TYPES_DATETIME]);

    UA_String source_name = UA_STRING((char*)"Gateway");

    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, (char*)"SourceName"), &source_name, &UA_TYPES[UA_TYPES_STRING]);

    int priority = t_alarm_obj.HasMember("priority") ? t_alarm_obj["priority"].GetInt() : 0;

    UA_UInt16 severity = 100;

    if (priority == 1)
        severity = 300;
    else if (priority == 2)
        severity = 500;
    else if (priority == 3)
        severity = 900;

    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, (char*)"Severity"), &severity, &UA_TYPES[UA_TYPES_UINT16]);

    std::string msg = fmt::format(
        "Alarm {} triggered on DB{}.{}", t_alarm_obj.HasMember("code") ? t_alarm_obj["code"].GetUint() : 0, t_db_number, t_path);

    UA_LocalizedText message = UA_LOCALIZEDTEXT((char*)"en-US", (char*)msg.c_str());

    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, (char*)"Message"), &message, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);

    if (t_alarm_obj.HasMember("code")) {

        UA_UInt16 code = static_cast<UA_UInt16>(t_alarm_obj["code"].GetUint());

        UA_Server_writeObjectProperty_scalar(
            p_raw, event_node_id, UA_QUALIFIEDNAME(1, (char*)"AlarmCode"), &code, &UA_TYPES[UA_TYPES_UINT16]);
    }

    UA_Byte uaprio = static_cast<UA_Byte>(priority);

    UA_Server_writeObjectProperty_scalar(p_raw, event_node_id, UA_QUALIFIEDNAME(1, (char*)"Priority"), &uaprio, &UA_TYPES[UA_TYPES_BYTE]);

    UA_Server_triggerEvent(p_raw, event_node_id, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), nullptr, UA_TRUE);
}

} // namespace sgrn::gateway::adapters
