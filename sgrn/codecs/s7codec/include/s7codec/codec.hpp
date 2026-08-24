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
 * @file  s7/codec.hpp
 * @brief Scalar encode/decode for all S7 data types on raw uint8_t* buffers.
 */

#pragma once

#include "datetime.hpp"
#include "endian.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <type_traits>
#include <variant>

namespace s7codec
{

enum class ValueKind : size_t { None = 0, Bool, SignedInt, UnsignedInt, Float, Double, String };

class DecodedValue : public std::variant<std::monostate, bool, int64_t, uint64_t, float, double, std::string> {
public:
    using Base = std::variant<std::monostate, bool, int64_t, uint64_t, float, double, std::string>;
    using Base::Base;
    using Base::operator=;

    // kind() lines up exactly with variant index() since the alternative
    // order above matches the old ValueKind enum order.
    ValueKind kind() const {
        return static_cast<ValueKind>(index());
    }
    bool valid() const {
        return index() != 0;
    }

    static DecodedValue makeBool(bool v) {
        return DecodedValue{v};
    }
    static DecodedValue makeSigned(int64_t v) {
        return DecodedValue{v};
    }
    static DecodedValue makeUnsigned(uint64_t v) {
        return DecodedValue{v};
    }
    static DecodedValue makeFloat(float v) {
        return DecodedValue{v};
    }
    static DecodedValue makeDouble(double v) {
        return DecodedValue{v};
    }
    static DecodedValue makeString(const std::string& v) {
        return DecodedValue{v};
    }
    static DecodedValue makeString(std::string&& v) {
        return DecodedValue{std::move(v)};
    }

    double asDouble() const {
        return std::visit(
            [](auto&& v) -> double {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>)
                    return v ? 1.0 : 0.0;
                else if constexpr (std::is_arithmetic_v<T>)
                    return static_cast<double>(v);
                else
                    return 0.0;
            },
            static_cast<const Base&>(*this));
    }

    int64_t asInt64() const {
        return std::visit(
            [](auto&& v) -> int64_t {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>)
                    return v ? 1 : 0;
                else if constexpr (std::is_arithmetic_v<T>)
                    return static_cast<int64_t>(v);
                else
                    return 0;
            },
            static_cast<const Base&>(*this));
    }

    // Field-shaped accessors so callsites only need to add "()".
    // std::get throws bad_variant_access on mismatch, same trust contract
    // the old code had (callers always checked kind()/valid() first).
    bool b() const {
        return std::get<bool>(*this);
    }
    int64_t i() const {
        return std::get<int64_t>(*this);
    }
    uint64_t u() const {
        return std::get<uint64_t>(*this);
    }
    float f() const {
        return std::get<float>(*this);
    }
    double d() const {
        return std::get<double>(*this);
    }
    const std::string& s() const {
        return std::get<std::string>(*this);
    }
};

template <typename Visitor>
decltype(auto) visit(const DecodedValue& dv, Visitor&& vis) {
    return std::visit(std::forward<Visitor>(vis), static_cast<const DecodedValue::Base&>(dv));
}

// small helper so lambdas can be written as an overload set
template <typename... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};
template <typename... Fs>
overloaded(Fs...) -> overloaded<Fs...>;

struct DtlComponents {
    uint16_t year{0};
    uint8_t month{0};
    uint8_t day{0};
    uint8_t day_of_week{0};
    uint8_t hour{0};
    uint8_t minute{0};
    uint8_t second{0};
    uint32_t nanosecond{0};
};

// All error outcomes that can flow out of the encode/decode primitives
// below. Every std::expected<void, ...> in this file now returns this
// enum instead of a std::string, and toString() below is the single
// place that maps a status back to a human-readable message.
enum class CodecStatus {
    INVALID_BIT_INDEX,
    BUFFER_TOO_SMALL,
    STRING_TOO_LONG,
    INVALID_UTF8,
    INVALID_VALUE,
    INVALID_DATE_STRING,
    INVALID_DATETIME_STRING,
    INVALID_DTL_STRING,
    UNSUPPORTED_TYPE,
};

// Static translator: CodecStatus -> human-readable message.
inline const char* toString(CodecStatus status) {
    switch (status) {
        case CodecStatus::INVALID_BIT_INDEX:
            return "bit_index must be 0-7";
        case CodecStatus::BUFFER_TOO_SMALL:
            return "buffer too small";
        case CodecStatus::STRING_TOO_LONG:
            return "string length exceeds max";
        case CodecStatus::INVALID_UTF8:
            return "invalid UTF-8 string";
        case CodecStatus::INVALID_VALUE:
            return "invalid value";
        case CodecStatus::INVALID_DATE_STRING:
            return "invalid Date string";
        case CodecStatus::INVALID_DATETIME_STRING:
            return "invalid DateTime string";
        case CodecStatus::INVALID_DTL_STRING:
            return "invalid DTL string";
        case CodecStatus::UNSUPPORTED_TYPE:
            return "unsupported type for scalar encode";
    }
    return "unknown codec error";
}

// --- Low-level Primitives ---

inline std::expected<void, CodecStatus> encodeBool(bool t_value, uint8_t t_bit_index, uint8_t* t_ptr, size_t t_buffer_size) {
    if (t_buffer_size < 1)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    if (t_bit_index > 7)
        return std::unexpected(CodecStatus::INVALID_BIT_INDEX);
    if (t_value)
        t_ptr[0] |= static_cast<uint8_t>(1u << t_bit_index);
    else
        t_ptr[0] &= static_cast<uint8_t>(~(1u << t_bit_index));
    return {};
}

inline std::expected<void, CodecStatus> encodeU8(uint8_t t_value, uint8_t* t_ptr, size_t t_buffer_size) {
    if (t_buffer_size < 1)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    t_ptr[0] = t_value;
    return {};
}

inline std::expected<void, CodecStatus> encodeI8(int8_t t_value, uint8_t* t_ptr, size_t t_buffer_size) {
    if (t_buffer_size < 1)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    t_ptr[0] = static_cast<uint8_t>(t_value);
    return {};
}

inline std::expected<void, CodecStatus> encodeI16(int16_t t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 2)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<int16_t>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeU16(uint16_t t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 2)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<uint16_t>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeI32(int32_t t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 4)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<int32_t>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeU32(uint32_t t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 4)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<uint32_t>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeI64(int64_t t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 8)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<int64_t>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeU64(uint64_t t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 8)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<uint64_t>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeReal(float t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 4)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<float>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeLReal(double t_value, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 8)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<double>(t_value, t_ptr, t_endian);
    return {};
}

inline std::expected<void, CodecStatus> encodeString(
    const char* t_value, uint32_t t_value_len, uint32_t t_max_len, uint8_t* t_ptr, size_t t_buffer_size) {
    if (t_buffer_size < static_cast<size_t>(2 + t_max_len))
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    if (t_value_len > t_max_len)
        return std::unexpected(CodecStatus::STRING_TOO_LONG);
    t_ptr[0] = static_cast<uint8_t>(t_max_len);
    t_ptr[1] = static_cast<uint8_t>(t_value_len);
    std::memcpy(t_ptr + 2, t_value, static_cast<std::size_t>(t_value_len));
    // Zero the tail bytes to avoid stale data being sent to the PLC
    std::memset(t_ptr + 2 + t_value_len, 0, static_cast<size_t>(t_max_len - t_value_len));
    return {};
}

inline std::expected<void, CodecStatus> encodeWString(const uint16_t* t_code_units, uint32_t t_num_units, uint32_t t_max_len,
    uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < static_cast<size_t>(4 + t_max_len * 2))
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    if (t_num_units > t_max_len)
        return std::unexpected(CodecStatus::STRING_TOO_LONG);
    toEndian<uint16_t>(static_cast<uint16_t>(t_max_len), t_ptr, t_endian);
    toEndian<uint16_t>(static_cast<uint16_t>(t_num_units), t_ptr + 2, t_endian);
    for (uint32_t i = 0; i < t_num_units; ++i) {
        toEndian<uint16_t>(t_code_units[i], t_ptr + 4 + (i * 2), t_endian);
    }
    uint32_t tail_bytes = (t_max_len - t_num_units) * 2;
    if (tail_bytes > 0)
        std::memset(t_ptr + 4 + (t_num_units * 2), 0, static_cast<size_t>(tail_bytes));
    return {};
}

inline std::expected<void, CodecStatus> encodeXString(
    const char* t_value, uint32_t t_value_len, uint32_t t_max_len, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < static_cast<size_t>(8 + t_max_len))
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    if (t_value_len > t_max_len)
        return std::unexpected(CodecStatus::STRING_TOO_LONG);
    toEndian<uint32_t>(t_max_len, t_ptr, t_endian);
    toEndian<uint32_t>(t_value_len, t_ptr + 4, t_endian);
    std::memcpy(t_ptr + 8, t_value, static_cast<std::size_t>(t_value_len));
    std::memset(t_ptr + 8 + t_value_len, 0, static_cast<size_t>(t_max_len - t_value_len));
    return {};
}

inline std::expected<void, CodecStatus> encodeXWString(const uint16_t* t_code_units, uint32_t t_num_units, uint32_t t_max_len,
    uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < static_cast<size_t>(8 + t_max_len * 2))
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    if (t_num_units > t_max_len)
        return std::unexpected(CodecStatus::STRING_TOO_LONG);
    toEndian<uint32_t>(t_max_len, t_ptr, t_endian);
    toEndian<uint32_t>(t_num_units, t_ptr + 4, t_endian);
    for (uint32_t i = 0; i < t_num_units; ++i) {
        toEndian<uint16_t>(t_code_units[i], t_ptr + 8 + (i * 2), t_endian);
    }
    uint32_t tail_bytes = (t_max_len - t_num_units) * 2;
    if (tail_bytes > 0)
        std::memset(t_ptr + 8 + (t_num_units * 2), 0, static_cast<size_t>(tail_bytes));
    return {};
}

inline std::expected<void, CodecStatus> encodeDateTime(
    int t_year, int t_month, int t_day, int t_hour, int t_minute, int t_second, int t_day_of_week, uint8_t* t_ptr, size_t t_buffer_size) {
    if (t_buffer_size < 8)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    t_ptr[0] = decToBcd(t_year % 100);
    t_ptr[1] = decToBcd(t_month);
    t_ptr[2] = decToBcd(t_day);
    t_ptr[3] = decToBcd(t_hour);
    t_ptr[4] = decToBcd(t_minute);
    t_ptr[5] = decToBcd(t_second);
    t_ptr[6] = 0;
    t_ptr[7] = static_cast<uint8_t>(t_day_of_week);
    return {};
}

inline std::expected<void, CodecStatus> encodeDtl(
    const DtlComponents& t_components, uint8_t* t_ptr, size_t t_buffer_size, Endian t_endian = Endian::Big) {
    if (t_buffer_size < 12)
        return std::unexpected(CodecStatus::BUFFER_TOO_SMALL);
    toEndian<uint16_t>(t_components.year, t_ptr, t_endian);
    t_ptr[2] = t_components.month;
    t_ptr[3] = t_components.day;
    t_ptr[4] = t_components.day_of_week;
    t_ptr[5] = t_components.hour;
    t_ptr[6] = t_components.minute;
    t_ptr[7] = t_components.second;
    toEndian<uint32_t>(t_components.nanosecond, t_ptr + 8, t_endian);
    return {};
}

inline bool parseTimeString(const char* raw, int64_t& out_ms) {
    const char* p = raw;
    while (*p == ' ' || *p == '\t')
        ++p;
    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    }
    if (p[0] == '#' && (p[1] == 'T' || p[1] == 't'))
        p += 2;
    unsigned h = 0, m = 0, s = 0, ms = 0;
    int matched = std::sscanf(p, "%u:%u:%u.%u", &h, &m, &s, &ms);
    if (matched < 3)
        matched = std::sscanf(p, "%u:%u:%u:%u", &h, &m, &s, &ms);
    if (matched < 3)
        return false;
    int64_t total =
        static_cast<int64_t>(h) * 3600000 + static_cast<int64_t>(m) * 60000 + static_cast<int64_t>(s) * 1000 + static_cast<int64_t>(ms);
    out_ms = negative ? -total : total;
    return true;
}

inline std::string formatTimeString(int32_t ms_total) {
    bool negative = ms_total < 0;
    uint32_t abs_ms = negative ? static_cast<uint32_t>(-static_cast<int64_t>(ms_total)) : static_cast<uint32_t>(ms_total);
    uint32_t ms = abs_ms % 1000u;
    uint32_t secs = (abs_ms / 1000u) % 60u;
    uint32_t mins = (abs_ms / 60000u) % 60u;
    uint32_t hrs = abs_ms / 3600000u;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s#T%02u:%02u:%02u.%03u", negative ? "-" : "", hrs, mins, secs, ms);
    return std::string(buf);
}

inline std::string formatDecodedValue(const DecodedValue& t_decoded_value, Type t_type = Type::Byte) {
    if (!t_decoded_value.valid())
        return "null";
    if (t_type == Type::Bool)
        return t_decoded_value.asInt64() != 0 ? "true" : "false";
    return std::visit(
        [&](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, int64_t>)
                return t_type == Type::Time ? formatTimeString(static_cast<int32_t>(v)) : std::to_string(v);
            else if constexpr (std::is_same_v<T, uint64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, float>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return std::to_string(v);
            else /* std::string */
                return "\"" + v + "\"";
        },
        t_decoded_value);
}

// --- Main API ---

inline DecodedValue decodeScalar(
    Type t_type, const uint8_t* t_ptr, size_t t_buffer_size, uint8_t t_bit_index = 0, uint32_t t_count = 0, Endian t_endian = Endian::Big) {
    auto check = [&](size_t needed) -> bool { return t_buffer_size >= needed; };
    switch (t_type) {
        case Type::Bool:
            if (!check(1))
                return {};
            return DecodedValue::makeBool(static_cast<bool>((t_ptr[0] >> t_bit_index) & 0x01));
        case Type::Byte:
        case Type::USInt:
        case Type::Char:
            if (!check(1))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(t_ptr[0]));
        case Type::SInt:
            if (!check(1))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(static_cast<int8_t>(t_ptr[0])));
        case Type::Int:
            if (!check(2))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int16_t>(t_ptr, t_endian)));
        case Type::UInt:
        case Type::Word:
            if (!check(2))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint16_t>(t_ptr, t_endian)));
        case Type::Date:
            if (!check(2))
                return {};
            return DecodedValue::makeString(Date(fromEndian<uint16_t>(t_ptr, t_endian)).toString());
        case Type::WChar:
            if (!check(2))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint16_t>(t_ptr, t_endian)));
        case Type::DInt:
            if (!check(4))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int32_t>(t_ptr, t_endian)));
        case Type::DateTime: {
            if (!check(8))
                return {};
            DateTime dt;
            std::memcpy(dt.data, t_ptr, 8);
            return DecodedValue::makeString(dt.toString());
        }
        case Type::Time:
            if (!check(4))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int32_t>(t_ptr, t_endian)));
        case Type::UDInt:
        case Type::DWord:
            if (!check(4))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint32_t>(t_ptr, t_endian)));
        case Type::TimeOfDay:
            if (!check(4))
                return {};
            return DecodedValue::makeString(TOD(fromEndian<uint32_t>(t_ptr, t_endian)).toString());
        case Type::LInt:
            if (!check(8))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int64_t>(t_ptr, t_endian)));
        case Type::ULInt:
        case Type::LWord:
            if (!check(8))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint64_t>(t_ptr, t_endian)));
        case Type::LTime:
            if (!check(8))
                return {};
            return DecodedValue::makeString(LTime(fromEndian<int64_t>(t_ptr, t_endian)).toString());
        case Type::LTimeOfDay:
            if (!check(8))
                return {};
            return DecodedValue::makeString(LTOD(fromEndian<uint64_t>(t_ptr, t_endian)).toString());
        case Type::LDT:
        case Type::LDTL:
            if (!check(8))
                return {};
            // For now, represent LDT/LDTL as int64_t nanoseconds since 1970 for raw access.
            // SGRN OPC UA layer intercepts this to format as UA_DateTime.
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int64_t>(t_ptr, t_endian)));
        case Type::Real:
            if (!check(4))
                return {};
            return DecodedValue::makeFloat(fromEndian<float>(t_ptr, t_endian));
        case Type::LReal:
            if (!check(8))
                return {};
            return DecodedValue::makeDouble(fromEndian<double>(t_ptr, t_endian));
        case Type::DTL: {
            if (!check(12))
                return {};
            DTL dtl;
            dtl.year = fromEndian<uint16_t>(t_ptr, t_endian);
            dtl.month = t_ptr[2];
            dtl.day = t_ptr[3];
            dtl.hour = t_ptr[5];
            dtl.minute = t_ptr[6];
            dtl.second = t_ptr[7];
            dtl.nanosecond = fromEndian<uint32_t>(t_ptr + 8, t_endian);
            return DecodedValue::makeString(dtl.toString());
        }
        case Type::String: {
            if (!check(2))
                return {};
            uint8_t cur_len = t_ptr[1];
            uint32_t max_len = (t_count > 0 ? t_count : 254);
            if (cur_len > max_len)
                cur_len = static_cast<uint8_t>(max_len);
            if (!check(2 + cur_len))
                return {};
            return DecodedValue::makeString(std::string(reinterpret_cast<const char*>(&t_ptr[2]), cur_len));
        }
        case Type::WString: {
            if (!check(4))
                return {};
            uint32_t cur_len = static_cast<uint32_t>(fromEndian<uint16_t>(t_ptr + 2, t_endian));
            uint32_t max_len = (t_count > 0 ? t_count : 16382);
            if (cur_len > max_len)
                cur_len = max_len;
            if (!check(4 + cur_len * 2))
                return {};
            std::string result;
            result.reserve(static_cast<std::size_t>(cur_len) * 3);
            for (uint32_t i = 0; i < cur_len; ++i) {
                uint16_t ch = fromEndian<uint16_t>(t_ptr + 4 + (i * 2), t_endian);
                if (ch >= 0xD800u && ch <= 0xDBFFu && i + 1 < cur_len) {
                    uint16_t low = fromEndian<uint16_t>(t_ptr + 4 + ((i + 1) * 2), t_endian);
                    if (low >= 0xDC00u && low <= 0xDFFFu) {
                        uint32_t cp = 0x10000u + (static_cast<uint32_t>(ch - 0xD800u) << 10) + static_cast<uint32_t>(low - 0xDC00u);
                        result.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                        result.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                        result.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                        result.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                        ++i;
                        continue;
                    }
                }
                if (ch < 0x80u) {
                    result.push_back(static_cast<char>(ch));
                } else if (ch < 0x800u) {
                    result.push_back(static_cast<char>(0xC0u | (ch >> 6)));
                    result.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
                } else {
                    result.push_back(static_cast<char>(0xE0u | (ch >> 12)));
                    result.push_back(static_cast<char>(0x80u | ((ch >> 6) & 0x3Fu)));
                    result.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
                }
            }
            return DecodedValue::makeString(std::move(result));
        }
        case Type::XString: {
            if (!check(8))
                return {};
            uint32_t cur_len = fromEndian<uint32_t>(t_ptr + 4, t_endian);
            uint32_t max_len = (t_count > 0 ? t_count : fromEndian<uint32_t>(t_ptr, t_endian));
            if (cur_len > max_len)
                cur_len = max_len;
            if (!check(8 + cur_len))
                return {};
            return DecodedValue::makeString(std::string(reinterpret_cast<const char*>(&t_ptr[8]), cur_len));
        }
        case Type::XWString: {
            if (!check(8))
                return {};
            uint32_t cur_len = fromEndian<uint32_t>(t_ptr + 4, t_endian);
            uint32_t max_len = (t_count > 0 ? t_count : fromEndian<uint32_t>(t_ptr, t_endian));
            if (cur_len > max_len)
                cur_len = max_len;
            if (!check(8 + cur_len * 2))
                return {};
            std::string result;
            result.reserve(static_cast<std::size_t>(cur_len) * 3);
            for (uint32_t i = 0; i < cur_len; ++i) {
                uint16_t ch = fromEndian<uint16_t>(t_ptr + 8 + (i * 2), t_endian);
                if (ch >= 0xD800u && ch <= 0xDBFFu && i + 1 < cur_len) {
                    uint16_t low = fromEndian<uint16_t>(t_ptr + 8 + ((i + 1) * 2), t_endian);
                    if (low >= 0xDC00u && low <= 0xDFFFu) {
                        uint32_t cp = 0x10000u + (static_cast<uint32_t>(ch - 0xD800u) << 10) + static_cast<uint32_t>(low - 0xDC00u);
                        result.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                        result.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                        result.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                        result.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                        ++i;
                        continue;
                    }
                }
                if (ch < 0x80u) {
                    result.push_back(static_cast<char>(ch));
                } else if (ch < 0x800u) {
                    result.push_back(static_cast<char>(0xC0u | (ch >> 6)));
                    result.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
                } else {
                    result.push_back(static_cast<char>(0xE0u | (ch >> 12)));
                    result.push_back(static_cast<char>(0x80u | ((ch >> 6) & 0x3Fu)));
                    result.push_back(static_cast<char>(0x80u | (ch & 0x3Fu)));
                }
            }
            return DecodedValue::makeString(std::move(result));
        }
        case Type::Counter:
        case Type::Timer:
            if (!check(2))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromBE<uint16_t>(t_ptr)));
        default:
            return {};
    }
}

// Single source of truth for "how many characters can this string field hold".
// Every call site decoding String/WString/XString/XWString MUST go through
// this instead of passing count_ or a literal — that's what let the
// truncation bug keep reappearing in new call sites.
inline uint32_t stringDecodeCapacity(Type t_type, uint32_t t_count, uint32_t t_string_capacity) {
    const bool is_string = (t_type == Type::String || t_type == Type::WString || t_type == Type::XString || t_type == Type::XWString);
    if (!is_string)
        return t_count;
    return t_string_capacity > 0 ? t_string_capacity : t_count;
}

inline std::expected<void, CodecStatus> encodeScalar(const DecodedValue& t_decoded_value, Type t_type, uint8_t* t_ptr, size_t t_buffer_size,
    uint8_t t_bit_index = 0, uint32_t t_count = 0, Endian t_endian = Endian::Big) {
    if (!t_decoded_value.valid())
        return std::unexpected(CodecStatus::INVALID_VALUE);
    switch (t_type) {
        case Type::Bool:
            return encodeBool(t_decoded_value.asInt64() != 0, t_bit_index, t_ptr, t_buffer_size);
        case Type::Byte:
        case Type::USInt:
        case Type::Char:
            return encodeU8(static_cast<uint8_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size);
        case Type::SInt:
            return encodeI8(static_cast<int8_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size);
        case Type::Int:
            return encodeI16(static_cast<int16_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::UInt:
        case Type::Word:
        case Type::Counter:
        case Type::Timer:
            return encodeU16(static_cast<uint16_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::DInt:
        case Type::Time:
            return encodeI32(static_cast<int32_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::UDInt:
        case Type::DWord:
            return encodeU32(static_cast<uint32_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::LInt:
            return encodeI64(t_decoded_value.asInt64(), t_ptr, t_buffer_size, t_endian);
        case Type::ULInt:
        case Type::LWord:
            return encodeU64(static_cast<uint64_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::Real:
            return encodeReal(static_cast<float>(t_decoded_value.asDouble()), t_ptr, t_buffer_size, t_endian);
        case Type::LReal:
            return encodeLReal(t_decoded_value.asDouble(), t_ptr, t_buffer_size, t_endian);
        case Type::String:
            return encodeString(
                t_decoded_value.s().c_str(), static_cast<uint32_t>(t_decoded_value.s().length()), t_count, t_ptr, t_buffer_size);
        case Type::XString:
            return encodeXString(
                t_decoded_value.s().c_str(), static_cast<uint32_t>(t_decoded_value.s().length()), t_count, t_ptr, t_buffer_size, t_endian);
        case Type::WString: {
            std::u16string utf16;
            const std::string& input = t_decoded_value.s();
            bool ok = true;
            for (size_t i = 0; i < input.length(); ++i) {
                uint32_t cp = 0;
                uint8_t c = static_cast<uint8_t>(input[i]);
                if (c < 0x80) {
                    cp = c;
                } else if ((c & 0xe0) == 0xc0) {
                    if (i + 1 >= input.length()) {
                        ok = false;
                        break;
                    }
                    uint8_t b1 = static_cast<uint8_t>(input[++i]);
                    cp = ((c & 0x1f) << 6) | (b1 & 0x3f);
                } else if ((c & 0xf0) == 0xe0) {
                    if (i + 2 >= input.length()) {
                        ok = false;
                        break;
                    }
                    uint8_t b1 = static_cast<uint8_t>(input[++i]);
                    uint8_t b2 = static_cast<uint8_t>(input[++i]);
                    cp = ((c & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
                } else {
                    ok = false;
                    break;
                }
                utf16.push_back(static_cast<char16_t>(cp));
            }
            if (!ok)
                return std::unexpected(CodecStatus::INVALID_UTF8);
            return encodeWString(reinterpret_cast<const uint16_t*>(utf16.c_str()), static_cast<uint32_t>(utf16.size()), t_count, t_ptr,
                t_buffer_size, t_endian);
        }
        case Type::XWString: {
            std::u16string utf16;
            const std::string& input = t_decoded_value.s();
            bool ok = true;
            for (size_t i = 0; i < input.length(); ++i) {
                uint32_t cp = 0;
                uint8_t c = static_cast<uint8_t>(input[i]);
                if (c < 0x80) {
                    cp = c;
                } else if ((c & 0xe0) == 0xc0) {
                    if (i + 1 >= input.length()) {
                        ok = false;
                        break;
                    }
                    uint8_t b1 = static_cast<uint8_t>(input[++i]);
                    cp = ((c & 0x1f) << 6) | (b1 & 0x3f);
                } else if ((c & 0xf0) == 0xe0) {
                    if (i + 2 >= input.length()) {
                        ok = false;
                        break;
                    }
                    uint8_t b1 = static_cast<uint8_t>(input[++i]);
                    uint8_t b2 = static_cast<uint8_t>(input[++i]);
                    cp = ((c & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
                } else {
                    ok = false;
                    break;
                }
                utf16.push_back(static_cast<char16_t>(cp));
            }
            if (!ok)
                return std::unexpected(CodecStatus::INVALID_UTF8);
            return encodeXWString(reinterpret_cast<const uint16_t*>(utf16.c_str()), static_cast<uint32_t>(utf16.size()), t_count, t_ptr,
                t_buffer_size, t_endian);
        }
        case Type::DateTime: {
            if (t_decoded_value.kind() != ValueKind::String)
                return std::unexpected(CodecStatus::INVALID_DATETIME_STRING);
            int y, m, d, h, min, s, ms;
            if (std::sscanf(t_decoded_value.s().c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &m, &d, &h, &min, &s, &ms) < 6)
                return std::unexpected(CodecStatus::INVALID_DATETIME_STRING);
            return encodeDateTime(y, m, d, h, min, s, 0, t_ptr, t_buffer_size);
        }
        case Type::DTL: {
            if (t_decoded_value.kind() != ValueKind::String)
                return std::unexpected(CodecStatus::INVALID_DTL_STRING);
            DtlComponents c;
            if (std::sscanf(t_decoded_value.s().c_str(), "%hu-%hhu-%hhu %hhu:%hhu:%hhu.%u", &c.year, &c.month, &c.day, &c.hour, &c.minute,
                    &c.second, &c.nanosecond) < 6)
                return std::unexpected(CodecStatus::INVALID_DTL_STRING);
            return encodeDtl(c, t_ptr, t_buffer_size, t_endian);
        }
        case Type::Date: {
            if (t_decoded_value.kind() == ValueKind::String) {
                struct tm ti{};
                if (std::sscanf(t_decoded_value.s().c_str(), "%d-%d-%d", &ti.tm_year, &ti.tm_mon, &ti.tm_mday) == 3) {
                    ti.tm_year -= 1900;
                    ti.tm_mon -= 1;
                    return encodeU16(Date(ti).get(), t_ptr, t_buffer_size, t_endian);
                }
                return std::unexpected(CodecStatus::INVALID_DATE_STRING);
            }
            return encodeU16(static_cast<uint16_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        }
        case Type::TimeOfDay:
            return encodeU32(static_cast<uint32_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::LTimeOfDay:
            return encodeU64(static_cast<uint64_t>(t_decoded_value.asInt64()), t_ptr, t_buffer_size, t_endian);
        case Type::LTime:
        case Type::LDT:
        case Type::LDTL:
            return encodeI64(t_decoded_value.asInt64(), t_ptr, t_buffer_size, t_endian);
        default:
            return std::unexpected(CodecStatus::UNSUPPORTED_TYPE);
    }
}

} // namespace s7codec
