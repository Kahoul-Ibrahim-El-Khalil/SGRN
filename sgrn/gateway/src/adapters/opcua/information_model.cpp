// sgrn/gateway/src/adapters/opcua/information_model.cpp

#include <fmt/core.h>

#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/information_model.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <sgrn/debug.hpp>
#include <algorithm>
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

// ── Internal helpers ─────────────────────────────────────────────────────────
// Everything in this namespace is a private decomposition of the five
// externally-declared functions below (see information_model.hpp). Kept
// static/anonymous-namespace so none of it leaks external linkage.
namespace
{

inline UA_StatusCode sgrn_UA_Server_write_attribute(
    UA_Server* server, const UA_NodeId* nodeId, UA_AttributeId attributeId, const UA_DataType* type, void* data) {
    UA_WriteValue wv;
    UA_WriteValue_init(&wv);
    wv.nodeId = *nodeId;
    wv.attributeId = attributeId;
    wv.value.hasValue = true;
    wv.value.value.type = type;
    wv.value.value.data = data;
    wv.value.value.storageType = UA_VARIANT_DATA;
    return UA_Server_write(server, &wv);
}

// Writes the DataTypeDefinition attribute (UA_EnumDefinition) for an enum
// DataType node — the machine-readable field/value list consumed by clients
// that support DataTypeDefinition browsing (OPC UA Part 3 §8.51).
Result<void, std::string> writeEnumDefinitionAttribute(UA_Server* tp_raw, const UA_DataType& t_type, const EnumTypeDef& t_def) {
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
        UA_StatusCode code = sgrn_UA_Server_write_attribute(
            tp_raw, &t_type.typeId, UA_ATTRIBUTEID_DATATYPEDEFINITION, &UA_TYPES[UA_TYPES_ENUMDEFINITION], &enum_def);
        if (code != UA_STATUSCODE_GOOD) {

            UA_EnumDefinition_clear(&enum_def);
            return fmt::format("Failed to register Enum {}, UA_StatusCode: {}", t_def.name, UA_StatusCode_name(code));
        }
    }
    UA_EnumDefinition_clear(&enum_def);
    return {};
}

static bool isContiguousZeroBasedEnum(const EnumTypeDef& t_def) {
    if (t_def.values.empty())
        return false;

    int expected = 0;
    for (const auto& [value, name] : t_def.values) {
        (void)name;
        if (value != expected)
            return false;
        ++expected;
    }
    return true;
}

Result<void, std::string> registerEnumStringsProperty(UA_Server* tp_raw, const UA_DataType& t_type, const EnumTypeDef& t_def) {
    SGRN_RETURN_IF(!isContiguousZeroBasedEnum(t_def), {});

    const size_t n = t_def.values.size();

    auto* lts = static_cast<UA_LocalizedText*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]));

    SGRN_RETURN_IF_NULL(!lts, {});

    size_t index = 0;
    for (const auto& [value, name] : t_def.values) {
        (void)value;
        lts[index++] = UA_LOCALIZEDTEXT_ALLOC("en", name.c_str());
    }

    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "EnumStrings");
    attr.dataType = UA_TYPES[UA_TYPES_LOCALIZEDTEXT].typeId;
    attr.valueRank = UA_VALUERANK_ONE_DIMENSION;
    attr.value.arrayLength = n;
    attr.value.arrayDimensions = nullptr;
    attr.value.arrayDimensionsSize = 0;

    UA_Variant_setArray(&attr.value, lts, n, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);

    const UA_NodeId property_id = UA_NODEID_STRING_ALLOC(1, (std::string(t_type.typeName) + ".EnumStrings").c_str());

    UA_Server_addVariableNode(tp_raw, property_id, t_type.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(0, const_cast<char*>("EnumStrings")), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, nullptr, nullptr);

    UA_NodeId_clear(const_cast<UA_NodeId*>(&property_id));
}

#if defined(UA_TYPES_ENUMVALUETYPE)
// EnumValues (value/displayName/description) — supported by open62541 v1.x.
// Independent of EnumStrings: each entry carries its own .value, so it's
// correct for sparse/negative enums where EnumStrings above bails out, and
// it never needs to share state with it.
void registerEnumValuesProperty(UA_Server* tp_raw, const UA_DataType& t_type, const EnumTypeDef& t_def) {
    const size_t n = t_def.values.size();
    UA_EnumValueType* evs = static_cast<UA_EnumValueType*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_ENUMVALUETYPE]));

    size_t idx = 0;
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
    v_attr.arrayDimensionsSize = 0;
    v_attr.arrayDimensions = nullptr;
    UA_Variant_copy(&values_var, &v_attr.value);

    UA_NodeId values_id = UA_NODEID_STRING_ALLOC(1, (std::string(t_type.typeName) + "_EnumValues").c_str());
    UA_Server_addVariableNode(tp_raw, values_id, t_type.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(0, "EnumValues"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), v_attr, nullptr, nullptr);

    UA_NodeId_clear(&values_id);
    UA_Variant_clear(&values_var);
}
#endif

// Registers a single custom UDT (non-enum) as a UA_DATATYPEKIND_STRUCTURE
// DataType node, plus its "Default Binary" encoding object.
void registerStructureDataType(UA_Server* p_raw, const UA_DataType& ut) {
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
        sgrn_UA_Server_write_attribute(p_raw, &ut.typeId, UA_ATTRIBUTEID_DATATYPEDEFINITION, &UA_TYPES[UA_TYPES_STRUCTUREDEFINITION], &sd);
    }

    UA_ObjectAttributes enc_attr = UA_ObjectAttributes_default;
    enc_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", "Default Binary");
    UA_Server_addObjectNode(p_raw, ut.binaryEncodingId, ut.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_HASENCODING),
        UA_QUALIFIEDNAME_ALLOC(0, "Default Binary"), UA_NODEID_NUMERIC(0, UA_NS0ID_DATATYPEENCODINGTYPE), enc_attr, nullptr, nullptr);

    UA_Array_delete(sd.fields, sd.fieldsSize, &UA_TYPES[UA_TYPES_STRUCTUREFIELD]);
}

// Shared shape for the two S7AlarmEventType properties (AlarmCode, Priority)
// — both were identical UA_VariableAttributes/addVariableNode calls that
// only differed in NodeId, name, and UA_DataType.
void addObjectTypeProperty(
    UA_Server* p_raw, const UA_NodeId& t_owner_type_id, UA_NodeId t_prop_id, const char* tp_name, const UA_DataType* tp_data_type) {
    UA_VariableAttributes v_attr = UA_VariableAttributes_default;
    v_attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    v_attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", tp_name);
    v_attr.dataType = tp_data_type->typeId;
    UA_Server_addVariableNode(p_raw, t_prop_id, t_owner_type_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME_ALLOC(1, tp_name), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), v_attr, nullptr, nullptr);
}

// Registers one DB's ObjectNode (folder + DBNumber property) and its field
// tree. `auto` param avoids hard-coding the exact PlcSchemaStore db-map
// value type here — swap for the concrete type name if you'd rather not
// have this be an implicit template.
void registerDbObjectNode(
    UA_Server* p_raw, const OpcUaAdapterContext& t_adapter, const OpcUaNodeRegistryContext& t_nodes, uint16_t t_num, const auto& t_db) {
    const std::string db_id_str = t_db.db_name.empty() ? fmt::format("DB{}", t_db.db_number) : t_db.db_name;

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
    const UA_UInt16 value = t_db.db_number;
    UA_Variant_setScalarCopy(&p_attr.value, &value, &UA_TYPES[UA_TYPES_UINT16]);
    UA_Server_addVariableNode(p_raw, prop_id, db_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), UA_QUALIFIEDNAME_ALLOC(0, "DBNumber"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), p_attr, nullptr, nullptr);
    UA_NodeId_clear(&prop_id);

    NodeId db_node = nodeIdFromRaw(db_id);
    OpcUaDbContext db_context{.number = t_num, .trigger_events = t_db.trigger_events};
    addFieldNodes(t_adapter, t_nodes, db_node, t_db.fields, "", db_id_str, db_context);

    UA_NodeId_clear(&db_id);
}

// Priority → Severity mapping for S7 alarms. Pulled out of triggerAlarmEvent
// so the mapping itself is independently testable/greppable.
UA_UInt16 severityForPriority(int t_priority) {
    switch (t_priority) {
        case 1:
            return 300;
        case 2:
            return 500;
        case 3:
            return 900;
        default:
            return 100;
    }
}

// Time / SourceName / Severity / Message — the properties every triggered
// event needs regardless of alarm-specific fields.
void writeBaseEventProperties(
    UA_Server* p_raw, const UA_NodeId& t_event_id, uint64_t t_timestamp_ms, UA_UInt16 t_severity, const std::string& t_message) {
    UA_DateTime event_time = UA_DateTime_fromUnixTime(static_cast<UA_Int64>(t_timestamp_ms / 1000)) +
                             static_cast<UA_DateTime>((t_timestamp_ms % 1000) * UA_DATETIME_MSEC);
    UA_Server_writeObjectProperty_scalar(p_raw, t_event_id, UA_QUALIFIEDNAME(0, (char*)"Time"), &event_time, &UA_TYPES[UA_TYPES_DATETIME]);

    UA_String source_name = UA_STRING((char*)"Gateway");
    UA_Server_writeObjectProperty_scalar(
        p_raw, t_event_id, UA_QUALIFIEDNAME(0, (char*)"SourceName"), &source_name, &UA_TYPES[UA_TYPES_STRING]);

    UA_UInt16 severity = t_severity;
    UA_Server_writeObjectProperty_scalar(p_raw, t_event_id, UA_QUALIFIEDNAME(0, (char*)"Severity"), &severity, &UA_TYPES[UA_TYPES_UINT16]);

    UA_LocalizedText message = UA_LOCALIZEDTEXT((char*)"en-US", const_cast<char*>(t_message.c_str()));
    UA_Server_writeObjectProperty_scalar(
        p_raw, t_event_id, UA_QUALIFIEDNAME(0, (char*)"Message"), &message, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
}

// AlarmCode / Priority — the S7AlarmEventType-specific properties.
void writeAlarmSpecificProperties(UA_Server* p_raw, const UA_NodeId& t_event_id, const rapidjson::Value& t_alarm_obj, int t_priority) {
    if (t_alarm_obj.HasMember("code")) {
        UA_UInt16 code = static_cast<UA_UInt16>(t_alarm_obj["code"].GetUint());
        UA_Server_writeObjectProperty_scalar(p_raw, t_event_id, UA_QUALIFIEDNAME(1, (char*)"AlarmCode"), &code, &UA_TYPES[UA_TYPES_UINT16]);
    }
    UA_Byte uaprio = static_cast<UA_Byte>(t_priority);
    UA_Server_writeObjectProperty_scalar(p_raw, t_event_id, UA_QUALIFIEDNAME(1, (char*)"Priority"), &uaprio, &UA_TYPES[UA_TYPES_BYTE]);
}

} // namespace

// ── Information model ───────────────────────────────────────────────────────

void registerEnumDataType(UA_Server* tp_raw, const UA_DataType& t_type, const EnumTypeDef& t_def) {
    UA_DataTypeAttributes attr = UA_DataTypeAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", t_type.typeName);

    UA_Server_addDataTypeNode(tp_raw, t_type.typeId, UA_NODEID_NUMERIC(0, UA_NS0ID_ENUMERATION), UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
        UA_QUALIFIEDNAME_ALLOC(1, t_type.typeName), attr, nullptr, nullptr);

    writeEnumDefinitionAttribute(tp_raw, t_type, t_def);

    if (t_def.values.empty())
        return;

    registerEnumStringsProperty(tp_raw, t_type, t_def);
#if defined(UA_TYPES_ENUMVALUETYPE)
    registerEnumValuesProperty(tp_raw, t_type, t_def);
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

    addObjectTypeProperty(p_raw, alarm_type_id, UA_NODEID_NUMERIC(1, 5001), "AlarmCode", &UA_TYPES[UA_TYPES_UINT16]);
    addObjectTypeProperty(p_raw, alarm_type_id, UA_NODEID_NUMERIC(1, 5002), "Priority", &UA_TYPES[UA_TYPES_BYTE]);

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

            if (p_def) {
                registerEnumDataType(p_raw, ut, *p_def);
            }

            continue;
        }

        registerStructureDataType(p_raw, ut);
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
        registerDbObjectNode(p_raw, adapter, nodes, num, db);
    }
}

void triggerAlarmEvent(Server& t_server, const NodeId& t_alarm_event_type_id, uint16_t t_db_number, const std::string& t_path,
    const rapidjson::Value& t_alarm_obj, uint64_t t_timestamp_ms) {
    if (!t_alarm_obj.IsObject())
        return;
    if (!t_alarm_obj.HasMember("active") || !t_alarm_obj["active"].IsBool() || !t_alarm_obj["active"].GetBool())
        return;

    (void)t_timestamp_ms;
    UA_Server* p_raw = t_server.raw();
    const int priority = t_alarm_obj.HasMember("priority") ? t_alarm_obj["priority"].GetInt() : 0;
    const std::string msg_str = fmt::format(
        "Alarm {} triggered on DB{}.{}", t_alarm_obj.HasMember("code") ? t_alarm_obj["code"].GetUint() : 0, t_db_number, t_path);

    UA_LocalizedText msg = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(msg_str.c_str()));
    UA_NodeId source_node = UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER);

    UA_Server_createEvent(p_raw, source_node, t_alarm_event_type_id.get(), severityForPriority(priority), msg, nullptr, nullptr, nullptr);
}

} // namespace sgrn::gateway::adapters
