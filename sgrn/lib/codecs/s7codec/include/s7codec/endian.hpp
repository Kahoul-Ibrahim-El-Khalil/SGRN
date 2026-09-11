/*
 * Copyright (C) 2026 Kahoul Ibrahim El-Khalil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file  s7/endian.hpp
 * @brief Portable big-endian encode/decode for S7 communication.
 *
 * ENDIAN NEUTRALITY
 * Industrial hardware (S7) is strictly Big-Endian, but modern compute (x86/ARM)
 * and simulators (PLCSIM) are often Little-Endian.
 * This layer abstracts physical byte-order, allowing the aggregator to remain
 * "Resilient" across heterogeneous hardware without code changes.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace s7codec
{
enum class Endian { Little, Big, Unknown };

#if defined(__cpp_lib_endian) || (defined(__cplusplus) && __cplusplus >= 202002L)
#include <bit>
inline constexpr Endian kNativeEndian = (std::endian::native == std::endian::little) ? Endian::Little
                                        : (std::endian::native == std::endian::big)  ? Endian::Big
                                                                                     : Endian::Unknown;
#elif defined(__BYTE_ORDER__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
inline constexpr Endian kNativeEndian = Endian::Little;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline constexpr Endian kNativeEndian = Endian::Big;
#else
inline constexpr Endian kNativeEndian = Endian::Unknown;
#endif
#elif defined(_WIN32) || defined(_WIN64)
inline constexpr Endian kNativeEndian = Endian::Little;
#else
inline constexpr Endian kNativeEndian = Endian::Unknown;
#endif

namespace detail
{

template <typename T>
inline void reverseBytes(T& val) {
    auto* b = reinterpret_cast<uint8_t*>(&val);
    for (std::size_t i = 0, j = sizeof(T) - 1; i < j; ++i, --j) {
        uint8_t tmp = b[i];
        b[i] = b[j];
        b[j] = tmp;
    }
}

} // namespace detail

template <typename T>
inline T fromBE(const uint8_t* p) {
    T val{};
    std::memcpy(&val, p, sizeof(T));
    if constexpr (kNativeEndian == Endian::Little || kNativeEndian == Endian::Unknown) {
        detail::reverseBytes(val);
    }
    return val;
}

template <typename T>
inline void toBE(T val, uint8_t* p) {
    if constexpr (kNativeEndian == Endian::Little || kNativeEndian == Endian::Unknown) {
        detail::reverseBytes(val);
    }
    std::memcpy(p, &val, sizeof(T));
}

template <typename T>
inline T fromLE(const uint8_t* p) {
    T val{};
    std::memcpy(&val, p, sizeof(T));
    if constexpr (kNativeEndian == Endian::Big || kNativeEndian == Endian::Unknown) {
        detail::reverseBytes(val);
    }
    return val;
}

template <typename T>
inline void toLE(T val, uint8_t* p) {
    if constexpr (kNativeEndian == Endian::Big || kNativeEndian == Endian::Unknown) {
        detail::reverseBytes(val);
    }
    std::memcpy(p, &val, sizeof(T));
}

/**
 * @brief Decode a value from a specified endianness to host order.
 */
template <typename T>
inline T fromEndian(const uint8_t* p, Endian e) {
    T val{};
    std::memcpy(&val, p, sizeof(T));
    if (e != kNativeEndian && e != Endian::Unknown) {
        detail::reverseBytes(val);
    }
    return val;
}

/**
 * @brief Encode a value from host order to a specified endianness.
 */
template <typename T>
inline void toEndian(T val, uint8_t* p, Endian e) {
    if (e != kNativeEndian && e != Endian::Unknown) {
        detail::reverseBytes(val);
    }
    std::memcpy(p, &val, sizeof(T));
}

inline constexpr int bcdToDec(uint8_t bcd) {
    return static_cast<int>((bcd >> 4) * 10 + (bcd & 0x0F));
}

inline constexpr uint8_t decToBcd(int dec) {
    return static_cast<uint8_t>(((dec / 10) << 4) | (dec % 10));
}

template <typename T>
struct alignas(2) BigEndian {
    static_assert(std::is_trivially_copyable_v<T>, "BigEndian<T> requires a trivially-copyable type");
    uint8_t bytes[sizeof(T)]{};
    BigEndian() = default;
    explicit BigEndian(T native) {
        store(native);
    }
    BigEndian& operator=(T native) {
        store(native);
        return *this;
    }
    operator T() const {
        return load();
    }
    T get() const {
        return load();
    }

private:
    void store(T native) {
        toBE<T>(native, bytes);
    }
    T load() const {
        return fromBE<T>(bytes);
    }
};

} // namespace s7codec
