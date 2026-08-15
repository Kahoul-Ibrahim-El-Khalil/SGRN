#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/information_model.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <fmt/core.h>
#include <functional>
#include <open62541/common.h>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

using ::sgrn::scl::DbField;

namespace sgrn::gateway::adapters
{

void registerEventTypes(wrappers::opcua::Server& t_server, wrappers::opcua::NodeId& t_alarm_event_type_id) {
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

    t_alarm_event_type_id = wrappers::opcua::nodeIdFromRaw(alarm_type_id);
}

void registerDataTypeNodes(wrappers::opcua::Server& t_server, const wrappers::opcua::TypeRegistry& t_type_registry) {
    UA_Server* p_raw = t_server.raw();

    for (const UA_DataType& ut : t_type_registry.types()) {
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

        if (res == UA_STATUSCODE_GOOD)
            __UA_Server_write(p_raw, &ut.typeId, UA_ATTRIBUTEID_DATATYPEDEFINITION, &UA_TYPES[UA_TYPES_STRUCTUREDEFINITION], &sd);

        UA_ObjectAttributes encAttr = UA_ObjectAttributes_default;
        encAttr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "Default Binary");
        UA_Server_addObjectNode(p_raw, ut.binaryEncodingId, ut.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASENCODING),
            UA_QUALIFIEDNAME_ALLOC(0, "Default Binary"), UA_NODEID_NUMERIC(0, UA_NS0ID_DATATYPEENCODINGTYPE), encAttr, nullptr, nullptr);

        UA_Array_delete(sd.fields, sd.fieldsSize, &UA_TYPES[UA_TYPES_STRUCTUREFIELD]);
    }
}

void buildAddressSpace(wrappers::opcua::Server& t_server, const ::sgrn::scl::PlcSchemaStore& t_registry, twin::PlcMemory* tp_s7_server,
    ::sgrn::gateway::SecurityManager* tp_security_manager, std::vector<std::unique_ptr<NodeContext>>& t_node_contexts,
    std::unordered_map<std::string, wrappers::opcua::NodeId>& t_node_id_map, wrappers::opcua::TypeRegistry& t_type_registry,
    wrappers::opcua::NodeId& t_alarm_event_type_id, DeltaPushHandler* tp_delta_push) {
    registerDataTypeNodes(t_server, t_type_registry);
    registerEventTypes(t_server, t_alarm_event_type_id);

    UA_Server* p_raw = t_server.raw();

    UA_Byte event_notifier = UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT;
    UA_Server_writeEventNotifier(p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), event_notifier);
    UA_Server_writeEventNotifier(p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), event_notifier);

    UA_Server_addReference(p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), UA_NODEID_NUMERIC(0, 48 /* HasNotifier */),
        UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), true);

    for (const auto& [num, db] : t_registry.dbs()) {
        const std::string db_id_str = db.db_name.empty() ? fmt::format("DB{}", db.db_number) : db.db_name;
        UA_NodeId dbId = UA_NODEID_STRING_ALLOC(1, db_id_str.c_str());
        UA_ObjectAttributes dbAttr = UA_ObjectAttributes_default;
        dbAttr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", db_id_str.c_str());
        dbAttr.eventNotifier = UA_EVENTNOTIFIER_SUBSCRIBE_TO_EVENT;
        UA_Server_addObjectNode(p_raw, dbId, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            UA_QUALIFIEDNAME_ALLOC(1, db_id_str.c_str()), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), dbAttr, nullptr, nullptr);

        UA_ExpandedNodeId dbExpId;
        UA_ExpandedNodeId_init(&dbExpId);
        dbExpId.nodeId = dbId;
        UA_Server_addReference(
            p_raw, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, 48 /* HasNotifier */), dbExpId, true);

        UA_NodeId propId = UA_NODEID_STRING_ALLOC(1, (db_id_str + ".DBNumber").c_str());
        UA_VariableAttributes pAttr = UA_VariableAttributes_default;
        pAttr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "DBNumber");
        const UA_UInt16 val = db.db_number;
        UA_Variant_setScalarCopy(&pAttr.value, &val, &UA_TYPES[UA_TYPES_UINT16]);
        UA_Server_addVariableNode(p_raw, propId, dbId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), UA_QUALIFIEDNAME_ALLOC(0, "DBNumber"),
            UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), pAttr, nullptr, nullptr);
        UA_NodeId_clear(&propId);

        wrappers::opcua::NodeId db_node = wrappers::opcua::nodeIdFromRaw(dbId);
        addFieldNodes(t_server, db_node, db.fields, "", db_id_str, num, tp_s7_server, tp_security_manager, t_node_contexts, &t_node_id_map,
            t_type_registry, db.trigger_events, tp_delta_push);
        UA_NodeId_clear(&dbId);
    }
}

void triggerAlarmEvent(wrappers::opcua::Server& t_server, const wrappers::opcua::NodeId& t_alarm_event_type_id, uint16_t t_db_number,
    const std::string& t_path, const rapidjson::Value& t_alarm_obj, uint64_t t_timestamp_ms) {
    if (!t_alarm_obj.IsObject())
        return;
    if (!t_alarm_obj.HasMember("active") || !t_alarm_obj["active"].IsBool() || !t_alarm_obj["active"].GetBool())
        return;

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
