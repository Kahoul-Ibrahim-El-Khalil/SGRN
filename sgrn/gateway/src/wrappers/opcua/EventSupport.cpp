#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/EventSupport.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>

#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

#include <fmt/core.h>

namespace sgrn::gateway::wrappers::opcua
{

sgrn::Result<NodeId> registerAlarmEventType(Server& t_server) {
    UA_Server* p_raw = t_server.raw();

    // Define a new ObjectType that inherits from BaseEventType (ns=0, id=2041)
    UA_NodeId base_event_type_id = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE);
    UA_NodeId alarm_type_id = UA_NODEID_NUMERIC(1, 5000); // private ns=1 id

    UA_ObjectTypeAttributes attr = UA_ObjectTypeAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT_ALLOC(const_cast<char*>("en"), const_cast<char*>("SgrnAlarmEventType"));
    attr.description = UA_LOCALIZEDTEXT_ALLOC(const_cast<char*>("en"), const_cast<char*>("Alarm event from PLC data block"));

    UA_StatusCode sc = UA_Server_addObjectTypeNode(p_raw, alarm_type_id, base_event_type_id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
        UA_QUALIFIEDNAME(1, const_cast<char*>("SgrnAlarmEventType")), attr, nullptr, nullptr);

    SGRN_RETURN_IF(sc != UA_STATUSCODE_GOOD, fmt::format("registerAlarmEventType: {}", UA_StatusCode_name(sc)));

    return nodeIdFromRaw(alarm_type_id);
}

void triggerAlarm(Server& t_server, const NodeId& t_event_type, uint16_t t_db, std::string_view t_path, const rapidjson::Value& t_alarm_obj,
    uint64_t t_timestamp_ms) {
    UA_Server* p_raw = t_server.raw();

    // Create a new event instance of our custom type
    UA_NodeId event_node_id = UA_NODEID_NULL;
    UA_StatusCode sc = UA_Server_createEvent(p_raw, t_event_type.get(), &event_node_id);
    if (sc != UA_STATUSCODE_GOOD)
        return;

    // ── SourceName: "DB<n>/<path>" ───────────────────────────────────────────
    std::string source_name = fmt::format("DB{}/{}", t_db, t_path);
    UA_String ua_source = UA_STRING(const_cast<char*>(source_name.c_str()));
    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, const_cast<char*>("SourceName")), &ua_source, &UA_TYPES[UA_TYPES_STRING]);

    // ── Time ─────────────────────────────────────────────────────────────────
    UA_DateTime event_time = millisToUaDateTime(t_timestamp_ms);
    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, const_cast<char*>("Time")), &event_time, &UA_TYPES[UA_TYPES_DATETIME]);

    // ── Severity (default 500 / medium) ─────────────────────────────────────
    UA_UInt16 severity = 500;
    if (t_alarm_obj.IsObject() && t_alarm_obj.HasMember("severity") && t_alarm_obj["severity"].IsUint())
        severity = static_cast<UA_UInt16>(t_alarm_obj["severity"].GetUint());
    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, const_cast<char*>("Severity")), &severity, &UA_TYPES[UA_TYPES_UINT16]);

    // ── Message ──────────────────────────────────────────────────────────────
    std::string msg_str;
    if (t_alarm_obj.IsObject() && t_alarm_obj.HasMember("message") && t_alarm_obj["message"].IsString())
        msg_str = t_alarm_obj["message"].GetString();
    else
        msg_str = fmt::format("Alarm at DB{}/{}", t_db, t_path);

    UA_LocalizedText msg = UA_LOCALIZEDTEXT(const_cast<char*>("en"), const_cast<char*>(msg_str.c_str()));
    UA_Server_writeObjectProperty_scalar(
        p_raw, event_node_id, UA_QUALIFIEDNAME(0, const_cast<char*>("Message")), &msg, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);

    // ── Fire ─────────────────────────────────────────────────────────────────
    UA_Server_triggerEvent(p_raw, event_node_id, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER), nullptr, UA_TRUE);
}

} // namespace sgrn::gateway::wrappers::opcua
