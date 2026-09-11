#pragma once

#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <string>

namespace sgrn::gateway::adapters::modbus::TypeTranslation
{

/**
 * @brief Decodes raw Modbus bytes (extracted from registers) into a string representation based on the target DataType.
 *
 * @param t_type The SCL generic data type (e.g. Bool, Int, Real).
 * @param t_useful_bytes The number of valid bytes in the registers.
 * @param tp_bytes Pointer to the array of bytes extracted from registers.
 * @return String representation of the decoded value.
 */
std::string decodeBytesToString(sgrn::scl::DataType t_type, int t_useful_bytes, const uint8_t* tp_bytes);

} // namespace sgrn::gateway::adapters::modbus::TypeTranslation
