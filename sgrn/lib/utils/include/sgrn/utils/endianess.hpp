#pragma once
/**
 * @file  sgrn/utils/endianess.hpp
 * @brief Byte-order utilities for S7 PLC communication.
 *
 * Delegates to the freestanding s7codec/endian.hpp library and provides
 * sgrn::utils:: namespace aliases for backward compatibility.
 */

#include <s7codec/endian.hpp>

#include <vector>

namespace sgrn::utils
{
// ══════════════════════════════════════════════════════════════════════════════
//  Byte-order utilities  (S7 PLCs are big-endian / Motorola byte order)
//  Delegates to s7codec:: — these wrappers preserve the existing API.
// ══════════════════════════════════════════════════════════════════════════════

template <typename T>
static T fromBigEndian(const uint8_t* tp_p) {
    return s7codec::fromBE<T>(tp_p);
}

template <typename T>
static T fromLittleEndian(const uint8_t* tp_p) {
    return s7codec::fromLE<T>(tp_p);
}

template <typename T>
static void toBigEndian(T t_val, std::vector<uint8_t>& t_out) {
    uint8_t buf[sizeof(T)];
    s7codec::toBE<T>(t_val, buf);
    t_out.insert(t_out.end(), buf, buf + sizeof(T));
}

template <typename T>
static void toBigEndian(T t_val, uint8_t* tp_p) {
    s7codec::toBE<T>(t_val, tp_p);
}

template <typename T>
static void toLittleEndian(T t_val, uint8_t* tp_p) {
    s7codec::toLE<T>(t_val, tp_p);
}

} // namespace sgrn::utils
