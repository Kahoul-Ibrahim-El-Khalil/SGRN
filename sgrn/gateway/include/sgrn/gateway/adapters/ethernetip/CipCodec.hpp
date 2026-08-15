#pragma once
#ifndef SGRN_GATEWAY_CIPCODEC_HPP
#define SGRN_GATEWAY_CIPCODEC_HPP

#include <sgrn/scl/types.hpp>
#include <cstddef>
#include <cstdint>

namespace sgrn::gateway::adapters::ethernetip
{

class CipCodec {
public:
    static size_t computeCipSize(const sgrn::scl::DbField& t_field);
    static void encodeToCip(const sgrn::scl::DbField& t_field, const uint8_t* tp_s7_buf, uint8_t* tp_cip_buf, size_t& t_cip_offset);
    static void decodeFromCip(const sgrn::scl::DbField& t_field, const uint8_t* tp_cip_buf, uint8_t* tp_s7_buf, size_t& t_cip_offset);

    /// Byte span of an S7 field in the arena (struct-aware).

    /// Decode a scalar S7 field from raw bytes into a typed CIP buffer.
    /// Returns false on unsupported type. Shared by handlePreGet and
    /// encodeToCip's scalar branch to keep the DataType→size mapping in one place.

    /// Encode a typed CIP buffer back into raw S7 bytes for a scalar field.
    /// Shared by handlePostSet and decodeFromCip's scalar branch.

private:
    static size_t alignOffset(size_t t_offset, size_t t_alignment);
};

} // namespace sgrn::gateway::adapters::ethernetip
#endif
