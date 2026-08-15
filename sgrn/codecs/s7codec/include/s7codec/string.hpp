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
 * @file  s7/string.hpp
 * @brief String and WString packed wrappers.
 */

#pragma once

#include "endian.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace s7codec
{

// ---------------------------------------------------------------------------
// String<MaxLen> — STRING[N]
// ---------------------------------------------------------------------------

template <uint8_t MaxLen>
struct alignas(2) String {
    static_assert(MaxLen >= 1 && MaxLen <= 254, "S7 STRING max length must be in [1, 254]");

    uint8_t max_length{MaxLen};
    uint8_t actual_length{0};
    char chars[MaxLen]{};

    String() = default;
    String(const char* s) {
        assign(s, std::strlen(s));
    }
    String(std::string_view sv) {
        assign(sv.data(), sv.size());
    }

    String& operator=(const char* s) {
        return assign(s, std::strlen(s));
    }
    String& operator=(std::string_view sv) {
        return assign(sv.data(), sv.size());
    }

    std::string_view view() const {
        return {chars, actual_length};
    }
    std::string toString() const {
        return std::string(chars, actual_length);
    }
    uint8_t size() const {
        return actual_length;
    }
    bool empty() const {
        return actual_length == 0;
    }
    static constexpr uint8_t capacity() {
        return MaxLen;
    }

    operator std::string_view() const {
        return view();
    }
    bool operator==(std::string_view rhs) const {
        return view() == rhs;
    }
    bool operator!=(std::string_view rhs) const {
        return !(*this == rhs);
    }

private:
    String& assign(const char* data, size_t len) {
        actual_length = static_cast<uint8_t>(std::min<size_t>(len, MaxLen));
        std::memcpy(chars, data, actual_length);
        if (actual_length < MaxLen)
            std::memset(chars + actual_length, 0, MaxLen - actual_length);
        return *this;
    }
};

// ---------------------------------------------------------------------------
// WString<MaxLen> — WSTRING[N]
// ---------------------------------------------------------------------------

template <uint16_t MaxLen>
struct alignas(2) WString {
    static_assert(MaxLen >= 1 && MaxLen <= 16382, "S7 WSTRING max length must be in [1, 16382]");

    BigEndian<uint16_t> max_length{MaxLen};
    BigEndian<uint16_t> actual_length{uint16_t{0}};
    BigEndian<uint16_t> chars[MaxLen]{};

    WString() = default;
    WString(const char16_t* s) {
        assign(s, u16len(s));
    }

    WString& operator=(const char16_t* s) {
        return assign(s, u16len(s));
    }

    std::u16string toString() const {
        uint16_t len = actual_length;
        std::u16string out(len, u'\0');
        for (uint16_t i = 0; i < len; ++i)
            out[i] = static_cast<char16_t>(chars[i].get());
        return out;
    }

    uint16_t size() const {
        return actual_length;
    }
    static constexpr uint16_t capacity() {
        return MaxLen;
    }

private:
    static size_t u16len(const char16_t* s) {
        size_t n = 0;
        while (s[n])
            ++n;
        return n;
    }

    WString& assign(const char16_t* data, size_t len) {
        uint16_t n = static_cast<uint16_t>(std::min<size_t>(len, MaxLen));
        actual_length = n;
        for (uint16_t i = 0; i < n; ++i)
            chars[i] = static_cast<uint16_t>(data[i]);
        for (uint16_t i = n; i < MaxLen; ++i)
            chars[i] = uint16_t{0};
        return *this;
    }
};

} // namespace s7codec
