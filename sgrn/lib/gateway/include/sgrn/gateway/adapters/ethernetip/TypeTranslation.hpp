#pragma once

#include <sgrn/scl/types.hpp>
#include <cstddef>
#include <cstdint>

namespace sgrn::gateway::adapters::ethernetip::TypeTranslation
{

/**
 * @brief Gets the CIP memory alignment requirement for a given field type.
 */
size_t getAlignment(const sgrn::scl::DbField& t_field);

/**
 * @brief Calculates the byte span needed for a field in S7 representation.
 */
size_t s7SpanBytes(const sgrn::scl::DbField& t_field);

/**
 * @brief Loads a scalar value from S7 raw bytes to CIP target memory based on field type.
 */
bool loadScalarFromS7(const sgrn::scl::DbField& t_field, const uint8_t* tp_s7_buf, void* tp_dst_buf);

/**
 * @brief Stores a scalar value from CIP memory to S7 raw bytes based on field type.
 */
bool storeScalarToS7(const sgrn::scl::DbField& t_field, const void* tp_src_buf, uint8_t* tp_s7_buf);

} // namespace sgrn::gateway::adapters::ethernetip::TypeTranslation
