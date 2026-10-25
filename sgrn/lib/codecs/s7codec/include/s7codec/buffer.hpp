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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file  s7codec/buffer.hpp
 * @brief Non-owning buffer views and semantic tag types shared by the
 *        s7codec primitives.
 *
 * ReadBuffer/WriteBuffer are std::span aliases: they give the codec a
 * bounds-checked handle while remaining non-owning (no allocations, no
 * copies). Historically the primitives took (uint8_t*, size_t) pairs;
 * those signatures remain available in codec.hpp as documented
 * forwarding adapters so existing call sites keep compiling.
 *
 * NOTE — real language requirements: the library already needs C++23
 * for std::expected (codec.hpp) and needs C++20 here for std::span,
 * despite the stale "C++17 freestanding" claims in s7.hpp/types.hpp.
 */

#pragma once

#include <cstdint>
#include <span>

namespace s7codec
{

/// Non-owning, bounds-checked read view (spans are never assumed large —
/// every consumer checks size() before indexing).
using ReadBuffer = std::span<const uint8_t>;

/// Non-owning, bounds-checked write view.
using WriteBuffer = std::span<uint8_t>;

/**
 * @brief Bit position within the FIRST byte of the buffer.
 *
 * Convention: LSB-first — bit N covers the value 0x01 << N of buffer[0].
 * On the wire this matches S7 Bool addressing (e.g. I0.5 / DB1.DBX0.5
 * occupy bit 5 of the first byte). Valid range is 0-7; encode-side
 * primitives enforce this and return CodecStatus::INVALID_BIT_INDEX
 * otherwise.
 */
struct BitOffset {
    uint8_t value{};

    constexpr BitOffset() = default;
    constexpr BitOffset(uint8_t v)
        : value(v) {
    }

    constexpr bool valid() const {
        return value <= 7;
    }
};

/**
 * @brief Maximum number of characters (String/XString) or code units
 *        (WString/XWString) a string field can hold.
 *
 * 0 means "use the builtin default for the type" (254 for String,
 * 16382 for WString, or the on-buffer header value for XString/XWString).
 */
struct StringCapacity {
    uint32_t value{};

    constexpr StringCapacity() = default;
    constexpr StringCapacity(uint32_t v)
        : value(v) {
    }

    constexpr bool usable() const {
        return value != 0;
    }
};

static_assert(sizeof(BitOffset) == 1 && sizeof(StringCapacity) == 4, "semantic types are thin wrappers");

} // namespace s7codec
