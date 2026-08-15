#pragma once

#include <cstdint>
#include <open62541/server.h>
#include <rapidjson/document.h>

namespace sgrn::gateway::adapters
{

struct NodeContext;

/// Convert a scalar RapidJSON value to a UA_DataValue (supports all primitive types)
bool jsonValueToDataValue(const rapidjson::Value& t_json_val, UA_DataValue& t_dv);

/// Convert a RapidJSON array to a typed UA_DataValue array
bool jsonArrayToDataValue(const rapidjson::Value& t_json_arr, int t_ua_type_idx, UA_DataValue& t_dv);

} // namespace sgrn::gateway::adapters
