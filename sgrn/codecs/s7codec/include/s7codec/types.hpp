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
 * @file  s7/types.hpp
 * @brief S7 data-type definitions — header-only, freestanding.
 *
 * UNIVERSAL PORTABILITY
 * This header-only library is "Freestanding" (zero dependencies, C++17).
 * It allows the EXACT same semantic mapping and byte-level logic to run on:
 *   - High-performance Linux/Windows Servers (x86_64/ARM64)
 *   - Resource-constrained Microcontrollers (ESP32, STM32, Arduino)
 * This eliminates the "Gateway Gap" by allowing the device to become its own mirror.
 *
 * @note  All functions are `inline` or `constexpr` to keep the library
 *        header-only without ODR violations.
 */

#pragma once

#include "endian.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace s7codec
{

// ---------------------------------------------------------------------------
// Type enumeration
// ---------------------------------------------------------------------------

/**
 * @brief Unified data-type enumeration for S7 PLCs.
 *
 * Maps every scalar and composite type exposed by Step7 / TIA Portal to a
 * single C++ enum.  This is the canonical source of truth; higher-level
 * libraries (SGRN, etc.) should alias or re-export this enum rather than
 * duplicating it.
 */
enum class Type {
    Bool,       ///< 1-bit boolean flag
    Byte,       ///< 8-bit unsigned integer (hex representation)
    Word,       ///< 16-bit unsigned integer (hex representation)
    DWord,      ///< 32-bit unsigned integer (hex representation)
    SInt,       ///< 8-bit signed integer
    USInt,      ///< 8-bit unsigned integer
    Int,        ///< 16-bit signed integer
    UInt,       ///< 16-bit unsigned integer
    DInt,       ///< 32-bit signed integer
    UDInt,      ///< 32-bit unsigned integer
    LInt,       ///< 64-bit signed integer
    ULInt,      ///< 64-bit unsigned integer
    LWord,      ///< 64-bit unsigned integer (hex representation)
    Real,       ///< 32-bit IEEE 754 floating point
    LReal,      ///< 64-bit IEEE 754 floating point
    Time,       ///< 32-bit signed milliseconds (timer)
    LTime,      ///< 64-bit signed nanoseconds
    Date,       ///< 16-bit unsigned days since 1990-01-01
    TimeOfDay,  ///< 32-bit unsigned ms since midnight
    LTimeOfDay, ///< 64-bit unsigned ns since midnight
    DateTime,   ///< 8-byte BCD encoded date and time
    DTL,        ///< 12-byte struct (TIA Portal Date and Time Long)
    Char,       ///< 8-bit ASCII character
    WChar,      ///< 16-bit Unicode character
    String,     ///< S7 String: 2-byte header [max_len, cur_len] + chars
    WString,    ///< S7 WString: 4-byte header + wchars
    XString,    ///< SGRN Extended String: 8-byte header [uint32 max, uint32 cur] + chars
    XWString,   ///< SGRN Extended WString: 8-byte header [uint32 max, uint32 cur] + wchars
    Counter,    ///< S7-300 Hardware Counter (16-bit BCD)
    Timer,      ///< S7-300 Hardware Timer (16-bit BCD)
    Struct      ///< Nested struct / array — decoded dynamically
};

// ---------------------------------------------------------------------------
// Type-name string constants
// ---------------------------------------------------------------------------

namespace detail
{

/// Uppercase a single ASCII character (constexpr-safe).
inline constexpr char toUpperChar(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
}

/// Case-insensitive comparison for two null-terminated C strings.
inline bool strEqualCI(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (toUpperChar(*a) != toUpperChar(*b))
            return false;
    }
    return *a == *b;
}

} // namespace detail

// ---------------------------------------------------------------------------
// s7TypeToString / stringToType
// ---------------------------------------------------------------------------

/**
 * @brief Returns a human-readable string for an Type enum value.
 */
inline const char* s7TypeToString(Type t) {
    switch (t) {
        case Type::Bool:
            return "BOOL";
        case Type::Byte:
            return "BYTE";
        case Type::Word:
            return "WORD";
        case Type::DWord:
            return "DWORD";
        case Type::SInt:
            return "SINT";
        case Type::USInt:
            return "USINT";
        case Type::Int:
            return "INT";
        case Type::UInt:
            return "UINT";
        case Type::DInt:
            return "DINT";
        case Type::UDInt:
            return "UDINT";
        case Type::LInt:
            return "LINT";
        case Type::ULInt:
            return "ULINT";
        case Type::LWord:
            return "LWORD";
        case Type::Real:
            return "REAL";
        case Type::LReal:
            return "LREAL";
        case Type::Time:
            return "TIME";
        case Type::LTime:
            return "LTIME";
        case Type::Date:
            return "DATE";
        case Type::TimeOfDay:
            return "TIMEOFDAY";
        case Type::LTimeOfDay:
            return "LTIMEOFDAY";
        case Type::DateTime:
            return "DATETIME";
        case Type::DTL:
            return "DTL";
        case Type::Char:
            return "CHAR";
        case Type::WChar:
            return "WCHAR";
        case Type::String:
            return "STRING";
        case Type::WString:
            return "WSTRING";
        case Type::XString:
            return "XSTRING";
        case Type::XWString:
            return "XWSTRING";
        case Type::Counter:
            return "COUNTER";
        case Type::Timer:
            return "TIMER";
        case Type::Struct:
            return "STRUCT";
    }
    return "Unknown";
}

/**
 * @brief Returns the C/C++ equivalent type name for an Type.
 */
inline const char* s7TypeToCType(Type t) {
    switch (t) {
        case Type::Bool:
            return "bool";
        case Type::Byte:
        case Type::USInt:
            return "uint8_t";
        case Type::SInt:
            return "int8_t";
        case Type::Char:
            return "char";
        case Type::Word:
        case Type::UInt:
        case Type::Date:
        case Type::Counter:
        case Type::Timer:
            return "uint16_t";
        case Type::Int:
            return "int16_t";
        case Type::WChar:
            return "uint16_t"; // UTF-16 unit
        case Type::DWord:
        case Type::UDInt:
        case Type::TimeOfDay:
            return "uint32_t";
        case Type::DInt:
        case Type::Time:
            return "int32_t";
        case Type::LWord:
        case Type::ULInt:
        case Type::LTimeOfDay:
            return "uint64_t";
        case Type::LInt:
        case Type::LTime:
            return "int64_t";
        case Type::Real:
            return "float";
        case Type::LReal:
            return "double";
        case Type::DateTime:
            return "S7DateTime"; // 8-byte BCD encoded timestamp
        case Type::DTL:
            return "struct DTL"; // 12-byte structural timestamp
        case Type::String:
            return "struct S7String"; // {uint8_t max; uint8_t len; char data[];}
        case Type::WString:
            return "struct S7WString"; // {uint16_t max; uint16_t len; uint16_t data[];}
        case Type::XString:
            return "struct SGRNXString"; // {uint32_t max; uint32_t len; char data[];}
        case Type::XWString:
            return "struct SGRNXWString"; // {uint32_t max; uint32_t len; uint16_t data[];}
        case Type::Struct:
            return "struct";
    }
    return "void";
}

/**
 * @brief Attempts to parse a type name string into an Type enum value.
 *
 * Accepts canonical names ("BOOL", "INT", ...) as well as common aliases
 * ("U8", "UINT8", "F32", "FLOAT", "LWORD", etc.).  Comparison is
 * case-insensitive.
 *
 * @param  name  Null-terminated string to parse.
 * @param  out   [out] Receives the resolved type on success.
 * @return `true` if the string was recognised, `false` otherwise.
 */
inline bool stringToType(const char* name, Type& out) {
    // Trim leading whitespace
    while (*name == ' ' || *name == '\t')
        ++name;

    // Fast-path dispatch based on first character (case-insensitive)
    const char first = detail::toUpperChar(*name);

    switch (first) {
        case 'B':
            if (detail::strEqualCI(name, "BOOL")) {
                out = Type::Bool;
                return true;
            }
            if (detail::strEqualCI(name, "BYTE")) {
                out = Type::Byte;
                return true;
            }
            break;
        case 'W':
            if (detail::strEqualCI(name, "WORD")) {
                out = Type::Word;
                return true;
            }
            if (detail::strEqualCI(name, "WCHAR")) {
                out = Type::WChar;
                return true;
            }
            if (detail::strEqualCI(name, "WSTRING")) {
                out = Type::WString;
                return true;
            }
            break;
        case 'D':
            if (detail::strEqualCI(name, "DWORD")) {
                out = Type::DWord;
                return true;
            }
            if (detail::strEqualCI(name, "DINT")) {
                out = Type::DInt;
                return true;
            }
            if (detail::strEqualCI(name, "DATE")) {
                out = Type::Date;
                return true;
            }
            if (detail::strEqualCI(name, "DATETIME")) {
                out = Type::DateTime;
                return true;
            }
            if (detail::strEqualCI(name, "DATE_AND_TIME")) {
                out = Type::DateTime;
                return true;
            }
            if (detail::strEqualCI(name, "DT")) {
                out = Type::DateTime;
                return true;
            }
            if (detail::strEqualCI(name, "DTL")) {
                out = Type::DTL;
                return true;
            }
            if (detail::strEqualCI(name, "DOUBLE")) {
                out = Type::LReal;
                return true;
            }
            break;
        case 'S':
            if (detail::strEqualCI(name, "SINT")) {
                out = Type::SInt;
                return true;
            }
            if (detail::strEqualCI(name, "STRING")) {
                out = Type::String;
                return true;
            }
            if (detail::strEqualCI(name, "STRUCT")) {
                out = Type::Struct;
                return true;
            }
            break;
        case 'U':
            if (detail::strEqualCI(name, "USINT")) {
                out = Type::USInt;
                return true;
            }
            if (detail::strEqualCI(name, "UINT")) {
                out = Type::UInt;
                return true;
            }
            if (detail::strEqualCI(name, "UDINT")) {
                out = Type::UDInt;
                return true;
            }
            if (detail::strEqualCI(name, "ULINT")) {
                out = Type::ULInt;
                return true;
            }
            if (detail::strEqualCI(name, "U8")) {
                out = Type::Byte;
                return true;
            }
            if (detail::strEqualCI(name, "UINT8")) {
                out = Type::Byte;
                return true;
            }
            if (detail::strEqualCI(name, "U16")) {
                out = Type::UInt;
                return true;
            }
            if (detail::strEqualCI(name, "UINT16")) {
                out = Type::UInt;
                return true;
            }
            if (detail::strEqualCI(name, "U32")) {
                out = Type::UDInt;
                return true;
            }
            if (detail::strEqualCI(name, "UINT32")) {
                out = Type::UDInt;
                return true;
            }
            if (detail::strEqualCI(name, "U64")) {
                out = Type::ULInt;
                return true;
            }
            if (detail::strEqualCI(name, "UINT64")) {
                out = Type::ULInt;
                return true;
            }
            break;
        case 'X':
            if (detail::strEqualCI(name, "XSTRING")) {
                out = Type::XString;
                return true;
            }
            if (detail::strEqualCI(name, "XWSTRING")) {
                out = Type::XWString;
                return true;
            }
            break;
        case 'I':
            if (detail::strEqualCI(name, "INT")) {
                out = Type::Int;
                return true;
            }
            if (detail::strEqualCI(name, "INT8")) {
                out = Type::SInt;
                return true;
            }
            if (detail::strEqualCI(name, "INT16")) {
                out = Type::Int;
                return true;
            }
            if (detail::strEqualCI(name, "INT32")) {
                out = Type::DInt;
                return true;
            }
            if (detail::strEqualCI(name, "INT64")) {
                out = Type::LInt;
                return true;
            }
            if (detail::strEqualCI(name, "I8")) {
                out = Type::SInt;
                return true;
            }
            if (detail::strEqualCI(name, "I16")) {
                out = Type::Int;
                return true;
            }
            if (detail::strEqualCI(name, "I32")) {
                out = Type::DInt;
                return true;
            }
            if (detail::strEqualCI(name, "I64")) {
                out = Type::LInt;
                return true;
            }
            break;
        case 'L':
            if (detail::strEqualCI(name, "LINT")) {
                out = Type::LInt;
                return true;
            }
            if (detail::strEqualCI(name, "LREAL")) {
                out = Type::LReal;
                return true;
            }
            if (detail::strEqualCI(name, "LWORD")) {
                out = Type::LWord;
                return true;
            }
            if (detail::strEqualCI(name, "LTIME")) {
                out = Type::LTime;
                return true;
            }
            if (detail::strEqualCI(name, "LTOD")) {
                out = Type::LTimeOfDay;
                return true;
            }
            break;
        case 'R':
            if (detail::strEqualCI(name, "REAL")) {
                out = Type::Real;
                return true;
            }
            if (detail::strEqualCI(name, "CHAR")) {
                out = Type::Char;
                return true;
            }
            if (detail::strEqualCI(name, "COUNTER")) {
                out = Type::Counter;
                return true;
            }
            break;
        case 'T':
            if (detail::strEqualCI(name, "TIME")) {
                out = Type::Time;
                return true;
            }
            if (detail::strEqualCI(name, "TIMER")) {
                out = Type::Timer;
                return true;
            }
            if (detail::strEqualCI(name, "TIMEOFDAY")) {
                out = Type::TimeOfDay;
                return true;
            }
            if (detail::strEqualCI(name, "TOD")) {
                out = Type::TimeOfDay;
                return true;
            }
            break;
        case 'F':
            if (detail::strEqualCI(name, "F32")) {
                out = Type::Real;
                return true;
            }
            if (detail::strEqualCI(name, "F64")) {
                out = Type::LReal;
                return true;
            }
            if (detail::strEqualCI(name, "FLOAT")) {
                out = Type::Real;
                return true;
            }
            break;
        case 'C':
            if (detail::strEqualCI(name, "CHAR")) {
                out = Type::Char;
                return true;
            }
            break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Type sizing
// ---------------------------------------------------------------------------

/**
 * @brief Returns the byte size of a single element of the given primitive type.
 *
 * Returns 0 for String, WString, and Struct (whose sizes depend on field
 * metadata that this freestanding header does not model).
 */
inline constexpr int primitiveSize(Type type) {
    switch (type) {
        case Type::Bool:
        case Type::Byte:
        case Type::SInt:
        case Type::USInt:
        case Type::Char:
            return 1;
        case Type::Int:
        case Type::UInt:
        case Type::Word:
        case Type::Date:
        case Type::WChar:
        case Type::Counter:
        case Type::Timer:
            return 2;
        case Type::DInt:
        case Type::UDInt:
        case Type::DWord:
        case Type::Real:
        case Type::Time:
        case Type::TimeOfDay:
            return 4;
        case Type::LInt:
        case Type::ULInt:
        case Type::LWord:
        case Type::LReal:
        case Type::LTime:
        case Type::LTimeOfDay:
            return 8;
        case Type::DateTime:
            return 8;
        case Type::DTL:
            return 12;
        case Type::String:
        case Type::WString:
        case Type::XString:
        case Type::XWString:
        case Type::Struct:
            return 0;
    }
    return 0;
}

/**
 * @brief Returns the byte span for a type with an optional element count.
 *
 * @param type   The S7 data type.
 * @param count  For String: max chars; for WString: max wchars;
 *               for arrays: number of elements.
 */
inline constexpr int typeSpanBytes(Type type, int count) {
    switch (type) {
        case Type::String:
            return 2 + (count > 0 ? count : 0);
        case Type::WString:
            return 4 + ((count > 0 ? count : 0) * 2);
        case Type::XString:
            return 8 + (count > 0 ? count : 0);
        case Type::XWString:
            return 8 + ((count > 0 ? count : 0) * 2);
        case Type::Struct:
            return 0; // Caller must use struct_size from schema metadata
        default: {
            int elem = primitiveSize(type);
            return (count > 1 ? count : 1) * elem;
        }
    }
}

/**
 * @brief Checks if a value fits within the range of target type T.
 */
template <typename T, typename U>
inline constexpr bool isInRange(U value) {
    // Use widened comparison to avoid signed/unsigned mismatch
    if constexpr (std::is_signed<U>::value && std::is_unsigned<T>::value) {
        if (value < 0)
            return false;
        return static_cast<uint64_t>(value) <= static_cast<uint64_t>((std::numeric_limits<T>::max)());
    } else if constexpr (std::is_unsigned<U>::value && std::is_signed<T>::value) {
        return value <= static_cast<uint64_t>((std::numeric_limits<T>::max)());
    } else {
        return value >= (std::numeric_limits<T>::min)() && value <= (std::numeric_limits<T>::max)();
    }
}

// ---------------------------------------------------------------------------
// Structural definitions (for memory mapping / reference)
// ---------------------------------------------------------------------------

/**
 * @brief Raw layout of an S7-1200/1500 DTL (12 bytes).
 */
struct alignas(4) S7RawDTL {
    uint16_t year{0};
    uint8_t month{1};
    uint8_t day{1};
    uint8_t weekday{1};
    uint8_t hour{0};
    uint8_t minute{0};
    uint8_t second{0};
    uint32_t nanosecond{0};

    S7RawDTL() = default;

    /**
     * @brief Initialize from an ISO 8601 string (e.g. "2024-03-18T19:30:00").
     */
    S7RawDTL(const char* iso) {
        int y = 0, m = 1, d = 1, h = 0, min = 0, s = 0;
        if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &min, &s) >= 3) {
            year = static_cast<uint16_t>(y);
            month = static_cast<uint8_t>(m);
            day = static_cast<uint8_t>(d);
            hour = static_cast<uint8_t>(h);
            minute = static_cast<uint8_t>(min);
            second = static_cast<uint8_t>(s);
        }
    }

    /**
     * @brief Create a DTL with the current system time (nanosecond precision).
     */
    static S7RawDTL now() {
        S7RawDTL dtl;
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto nanosecs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() % 1000000000;

        time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf{};
        struct tm* tm_info = nullptr;
#if defined(_WIN32) || defined(_WIN64)
        if (localtime_s(&tm_buf, &t) == 0)
            tm_info = &tm_buf;
#else
        tm_info = localtime_r(&t, &tm_buf);
#endif

        if (tm_info) {
            dtl.year = static_cast<uint16_t>(tm_info->tm_year + 1900);
            dtl.month = static_cast<uint8_t>(tm_info->tm_mon + 1);
            dtl.day = static_cast<uint8_t>(tm_info->tm_mday);
            dtl.weekday = static_cast<uint8_t>(tm_info->tm_wday + 1);
            dtl.hour = static_cast<uint8_t>(tm_info->tm_hour);
            dtl.minute = static_cast<uint8_t>(tm_info->tm_min);
            dtl.second = static_cast<uint8_t>(tm_info->tm_sec);
            dtl.nanosecond = static_cast<uint32_t>(nanosecs);
        }
        return dtl;
    }
};

/**
 * @brief Raw layout of an S7 String header and data.
 * @tparam N The maximum length of the string (default 254).
 */
template <size_t N = 254>
struct alignas(2) S7RawString {
    uint8_t max_len{static_cast<uint8_t>(N)};
    uint8_t cur_len{0};
    char data[N]{};

    S7RawString() = default;
    S7RawString(const char* s) {
        if (!s)
            return;
        size_t len = 0;
        while (s[len] && len < N) {
            data[len] = s[len];
            len++;
        }
        cur_len = static_cast<uint8_t>(len);
    }

    std::string_view str() const {
        return {data, cur_len};
    }
};

/**
 * @brief Raw layout of an S7 WString header and data.
 * @tparam N The maximum length of the string (default 254).
 */
template <size_t N = 254>
struct alignas(2) S7RawWString {
    uint16_t max_len{static_cast<uint16_t>(N)};
    uint16_t cur_len{0};
    uint16_t data[N]{};

    S7RawWString() = default;
    S7RawWString(const char16_t* s) {
        if (!s)
            return;
        size_t len = 0;
        while (s[len] && len < N) {
            data[len] = static_cast<uint16_t>(s[len]);
            len++;
        }
        cur_len = static_cast<uint16_t>(len);
    }
};

// ---------------------------------------------------------------------------
// High-level S7 Type Aliases (for packed structs)
// ---------------------------------------------------------------------------

using Bool = bool;
using Byte = uint8_t;
using USInt = uint8_t;
using SInt = int8_t;
using Char = char;

using Word = BigEndian<uint16_t>;
using UInt = BigEndian<uint16_t>;
using Int = BigEndian<int16_t>;
using WChar = BigEndian<uint16_t>;

using DWord = BigEndian<uint32_t>;
using UDInt = BigEndian<uint32_t>;
using DInt = BigEndian<int32_t>;
using Real = BigEndian<float>;
using Time = BigEndian<int32_t>;

using LWord = BigEndian<uint64_t>;
using ULInt = BigEndian<uint64_t>;
using LInt = BigEndian<int64_t>;
using LReal = BigEndian<double>;

/**
 * @brief Macro to define a S7 Data Block (DB) layout.
 *
 * This defines a struct representing a S7 DB layout without explicit struct packing,
 * allowing standard C/C++ struct semantics. All multi-byte core S7 types (e.g. Real, Int,
 * DTL, S7String) use `alignas(2)` alignment. This guarantees that they align to even addresses,
 * matching standard S7 unoptimized DB layout rules and ensuring 100% binary compatibility
 * with the offset calculation logic in `sgrn/s7/src/schema/DbSymbolsParser.cpp`.
 */
#define DATABLOCK(name) struct name

} // namespace s7codec
