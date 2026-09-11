#pragma once

#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/scl/types.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

// Forward declarations
using UA_Boolean = bool;

namespace sgrn::gateway::adapters
{

sgrn::Result<int, std::string> dataTypeToUaTypeIndex(::sgrn::scl::DataType t_type);
size_t boolArrayByteCount(size_t t_bit_count);
sgrn::Result<void, OpcUaAdapterError> writeBoolArrayToMemory(
    const UA_Boolean* tp_source, size_t t_count, uint8_t* tp_destination, size_t t_destination_size);

/// Stable key that identifies a scalar-backed OPC UA Enumeration type by the
/// S7 scalar it is derived from plus its ordered value→name map. Two fields that
/// share the same underlying scalar type and the same enum value set collapse to
/// a single registered `UA_DATATYPEKIND_ENUM`.
inline std::string enumTypeSignature(int t_ua_base_index, const std::map<int, std::string>& t_values) {
    std::string sig = std::to_string(t_ua_base_index) + ":";
    for (const auto& [k, v] : t_values)
        sig += std::to_string(k) + "=" + v + ";";
    return sig;
}

} // namespace sgrn::gateway::adapters
