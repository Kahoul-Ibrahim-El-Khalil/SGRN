#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>

#include <rapidjson/document.h>

#include <cstdint>
#include <string_view>

namespace sgrn::gateway::wrappers::opcua
{

class Server; // forward declaration — avoids circular include with Server.hpp

/// Register a custom AlarmConditionType-derived event type on the server.
/// Must be called after buildAddressSpace / after UA_Server_run_startup.
/// Returns the NodeId of the new event type node.
sgrn::Result<NodeId> registerAlarmEventType(Server& t_server);

/// Fire an alarm event of the registered type on the server.
///
/// @param server            The wrapper server instance.
/// @param event_type        NodeId returned by registerAlarmEventType().
/// @param db                PLC data-block number (used as event source identifier).
/// @param path              Dot-separated field path within the data-block.
/// @param alarm_obj         RapidJSON value describing the alarm payload.
///                          May be a scalar, object, or array element.
/// @param timestamp_ms      Unix epoch milliseconds for the event SourceTimestamp.
void triggerAlarm(Server& t_server, const NodeId& t_event_type, uint16_t t_db, std::string_view t_path, const rapidjson::Value& t_alarm_obj,
    uint64_t t_timestamp_ms);

} // namespace sgrn::gateway::wrappers::opcua
