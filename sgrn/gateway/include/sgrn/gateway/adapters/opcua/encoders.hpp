#pragma once

#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/gateway/adapters/opcua/scalar_view.hpp>
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
struct ArrayOfBoolsEncodingContext {
    const UA_Boolean* p_source;
    size_t count;
    uint8_t* p_destination;
    size_t destination_size;
};

struct OpcUaEncodingContext {
    const UA_DataType* p_ua_type;
    const uint8_t* p_ua_buf;
    uint8_t* p_memory;
    /// Scalar layout snapshot — cheap POD mirror of the PlcNode fields the
    /// scalar codec paths touch. Always populated (from p_node when one is
    /// provided), so scalar encoding never dereferences the full PlcNode.
    PlcScalarView view{};
    /// Non-null only for STRUCT payloads: children_ drives member recursion.
    const twin::PlcNode* p_node{nullptr};
    uint32_t override_size = 0;

    /// Recursion guard for nested/self-referential UDTs — encodeStructOpcUaToMemory
    /// increments this on every nested call and refuses to go past kMaxStructDepth.
    uint16_t depth = 0;
    /// Struct payload: keeps the full PlcNode (children_) AND snapshots its
    /// scalar layout into `view`. Existing call sites passing a PlcNode*
    /// resolve here.
    OpcUaEncodingContext(const UA_DataType* tp_ua_type, const uint8_t* tp_ua_buf, uint8_t* tp_memory, const twin::PlcNode* tp_node);
    /// Scalar-only payload from a prebuilt view (array-element hot loops —
    /// no strings/children_/atomic copies).
    OpcUaEncodingContext(const UA_DataType* tp_ua_type, const uint8_t* tp_ua_buf, uint8_t* tp_memory, const PlcScalarView& t_view)
        : p_ua_type(tp_ua_type)
        , p_ua_buf(tp_ua_buf)
        , p_memory(tp_memory)
        , view(t_view)
        , p_node(nullptr) {
    }

    uint32_t effectiveSize() const {
        return override_size > 0 ? override_size : view.size;
    }
};

Result<void, OpcUaAdapterError> encodeArrayOfBoolsToMemory(const ArrayOfBoolsEncodingContext& t_ctx);
Result<void, OpcUaAdapterError> encodeStringOpcUaToMemory(const OpcUaEncodingContext& t_ctx);
/**
 * @brief Encodes a single scalar OPC UA primitive into a raw S7 binary buffer.
 *
 * Maps open62541 UA_TYPES to s7codec primitives. Handles endianness conversions,
 * bit alignments for booleans, and boundary validation for critical types like
 * DTL and DateTime, preventing out-of-range epoch underflows.
 */
Result<void, OpcUaAdapterError> encodeScalarOpcUaToMemory(const OpcUaEncodingContext& t_ctx);
Result<void, OpcUaAdapterError> encodeStructOpcUaToMemory(const OpcUaEncodingContext& tp_ctx);
Result<void, OpcUaAdapterError> encodeTimeOfDayOpcUaToMemory(const OpcUaEncodingContext& tp_ctx);

} // namespace sgrn::gateway::adapters
