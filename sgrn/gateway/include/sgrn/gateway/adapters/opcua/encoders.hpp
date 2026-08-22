#pragma once

#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/udt_codec.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <fmt/core.h>
#include <opcua_codec_table.hpp>
#include <open62541/nodeids.h>
#include <open62541/server.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <vector>

namespace sgrn::gateway::adapters
{

/**
 * @brief Converts an OPC UA DateTime (UTC) to a local time structure.
 *
 * OPC UA transmits timestamps strictly in UTC. If written directly to PLC blocks (like DTL),
 * the SCADA/HMI will display the raw UTC time, causing timezone offset mismatches (e.g., 1-hour drift).
 * This helper resolves the UA_DateTime into the OS's local timezone (respecting DST rules)
 * before it is serialized to JSON or packed into S7 binary memory.
 */
inline struct tm resolveOpcUaLocalTime(UA_DateTime ua_date) {
    // UA_DateTime_toUnixTime() already returns Unix time in SECONDS.
    // Do NOT divide by 1000 again — that was an off-by-three-orders-of-magnitude bug.
    time_t t_sec = static_cast<time_t>(UA_DateTime_toUnixTime(ua_date));
    struct tm tm_info;
    localtime_r(&t_sec, &tm_info);
    return tm_info;
}

UA_StatusCode encodeStringOpcUaToMemory(const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const twin::PlcNode& t_node);
/**
 * @brief Encodes a single scalar OPC UA primitive into a raw S7 binary buffer.
 *
 * Maps open62541 UA_TYPES to s7codec primitives. Handles endianness conversions,
 * bit alignments for booleans, and boundary validation for critical types like
 * DTL and DateTime, preventing out-of-range epoch underflows.
 */

UA_StatusCode encodeScalarOpcUaToMemory(
    const UA_DataType* tp_ua_type, const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const twin::PlcNode& t_node);
UA_StatusCode translateOpcUaToMemory(const UA_DataType& t_type, const uint8_t* tp_ua_ptr, uint8_t* tp_memory, const twin::PlcNode& t_node);

} // namespace sgrn::gateway::adapters
