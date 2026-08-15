#pragma once

#include <sgrn/scl/types.hpp>
#include <cstddef>
#include <cstdint>

// Forward declarations
using UA_Boolean = bool;

namespace sgrn::gateway::adapters
{

int s7TypeToUaTypeIndex(::sgrn::scl::DataType t_type);
size_t boolArrayByteCount(size_t t_bit_count);
bool writeBoolArrayToS7(const UA_Boolean* tp_source, size_t t_count, uint8_t* tp_destination, size_t t_destination_size);

} // namespace sgrn::gateway::adapters
