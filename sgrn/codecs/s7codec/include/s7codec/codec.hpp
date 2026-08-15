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

struct CodecStatus {
    bool success{true};
    const char* message{""};
    bool ok() const {
        return success;
    }
    static CodecStatus Ok() {
        return {true, ""};
    }
    static CodecStatus Error(const char* msg) {
        return {false, msg};
    }
};

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

// --- Low-level Primitives ---

inline CodecStatus encodeBool(bool value, int bit_index, uint8_t* ptr, size_t buffer_size) {
    if (buffer_size < 1)
        return CodecStatus::Error("buffer too small for Bool");
    if (bit_index < 0 || bit_index > 7)
        return CodecStatus::Error("bit_index must be 0-7");
    if (value)
        ptr[0] |= static_cast<uint8_t>(1 << bit_index);
    else
        ptr[0] &= static_cast<uint8_t>(~(1 << bit_index));
    return CodecStatus::Ok();
}

inline CodecStatus encodeU8(uint8_t value, uint8_t* ptr, size_t buffer_size) {
    if (buffer_size < 1)
        return CodecStatus::Error("buffer too small for U8");
    ptr[0] = value;
    return CodecStatus::Ok();
}

inline CodecStatus encodeI8(int8_t value, uint8_t* ptr, size_t buffer_size) {
    if (buffer_size < 1)
        return CodecStatus::Error("buffer too small for I8");
    ptr[0] = static_cast<uint8_t>(value);
    return CodecStatus::Ok();
}

inline CodecStatus encodeI16(int16_t value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 2)
        return CodecStatus::Error("buffer too small for I16");
    toEndian<int16_t>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeU16(uint16_t value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 2)
        return CodecStatus::Error("buffer too small for U16");
    toEndian<uint16_t>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeI32(int32_t value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 4)
        return CodecStatus::Error("buffer too small for I32");
    toEndian<int32_t>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeU32(uint32_t value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 4)
        return CodecStatus::Error("buffer too small for U32");
    toEndian<uint32_t>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeI64(int64_t value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 8)
        return CodecStatus::Error("buffer too small for I64");
    toEndian<int64_t>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeU64(uint64_t value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 8)
        return CodecStatus::Error("buffer too small for U64");
    toEndian<uint64_t>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeReal(float value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 4)
        return CodecStatus::Error("buffer too small for Float");
    toEndian<float>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeLReal(double value, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 8)
        return CodecStatus::Error("buffer too small for Double");
    toEndian<double>(value, ptr, e);
    return CodecStatus::Ok();
}

inline CodecStatus encodeString(const char* value, int value_len, int max_len, uint8_t* ptr, size_t buffer_size) {
    if (buffer_size < static_cast<size_t>(2 + max_len))
        return CodecStatus::Error("buffer too small for String");
    if (value_len > max_len)
        return CodecStatus::Error("string length exceeds max");
    ptr[0] = static_cast<uint8_t>(max_len);
    ptr[1] = static_cast<uint8_t>(value_len);
    std::memcpy(ptr + 2, value, static_cast<std::size_t>(value_len));
    // Zero the tail bytes to avoid stale data being sent to the PLC
    std::memset(ptr + 2 + value_len, 0, static_cast<size_t>(max_len - value_len));
    return CodecStatus::Ok();
}

inline CodecStatus encodeWString(
    const uint16_t* code_units, int num_units, int max_len, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < static_cast<size_t>(4 + max_len * 2))
        return CodecStatus::Error("buffer too small for WString");
    if (num_units > max_len)
        return CodecStatus::Error("wstring length exceeds max");
    toEndian<uint16_t>(static_cast<uint16_t>(max_len), ptr, e);
    toEndian<uint16_t>(static_cast<uint16_t>(num_units), ptr + 2, e);
    for (int i = 0; i < num_units; ++i) {
        toEndian<uint16_t>(code_units[i], ptr + 4 + (i * 2), e);
    }
    int tail_bytes = (max_len - num_units) * 2;
    if (tail_bytes > 0)
        std::memset(ptr + 4 + (num_units * 2), 0, static_cast<size_t>(tail_bytes));
    return CodecStatus::Ok();
}

inline CodecStatus encodeXString(const char* value, int value_len, int max_len, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < static_cast<size_t>(8 + max_len))
        return CodecStatus::Error("buffer too small for XString");
    if (value_len > max_len)
        return CodecStatus::Error("string length exceeds max");
    toEndian<uint32_t>(static_cast<uint32_t>(max_len), ptr, e);
    toEndian<uint32_t>(static_cast<uint32_t>(value_len), ptr + 4, e);
    std::memcpy(ptr + 8, value, static_cast<std::size_t>(value_len));
    std::memset(ptr + 8 + value_len, 0, static_cast<size_t>(max_len - value_len));
    return CodecStatus::Ok();
}

inline CodecStatus encodeXWString(
    const uint16_t* code_units, int num_units, int max_len, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < static_cast<size_t>(8 + max_len * 2))
        return CodecStatus::Error("buffer too small for XWString");
    if (num_units > max_len)
        return CodecStatus::Error("wstring length exceeds max");
    toEndian<uint32_t>(static_cast<uint32_t>(max_len), ptr, e);
    toEndian<uint32_t>(static_cast<uint32_t>(num_units), ptr + 4, e);
    for (int i = 0; i < num_units; ++i) {
        toEndian<uint16_t>(code_units[i], ptr + 8 + (i * 2), e);
    }
    int tail_bytes = (max_len - num_units) * 2;
    if (tail_bytes > 0)
        std::memset(ptr + 8 + (num_units * 2), 0, static_cast<size_t>(tail_bytes));
    return CodecStatus::Ok();
}

inline CodecStatus encodeDateTime(
    int year, int month, int day, int hour, int minute, int second, int day_of_week, uint8_t* ptr, size_t buffer_size) {
    if (buffer_size < 8)
        return CodecStatus::Error("buffer too small for DateTime");
    ptr[0] = decToBcd(year % 100);
    ptr[1] = decToBcd(month);
    ptr[2] = decToBcd(day);
    ptr[3] = decToBcd(hour);
    ptr[4] = decToBcd(minute);
    ptr[5] = decToBcd(second);
    ptr[6] = 0;
    ptr[7] = static_cast<uint8_t>(day_of_week);
    return CodecStatus::Ok();
}

inline CodecStatus encodeDtl(const DtlComponents& c, uint8_t* ptr, size_t buffer_size, Endian e = Endian::Big) {
    if (buffer_size < 12)
        return CodecStatus::Error("buffer too small for DTL");
    toEndian<uint16_t>(c.year, ptr, e);
    ptr[2] = c.month;
    ptr[3] = c.day;
    ptr[4] = c.day_of_week;
    ptr[5] = c.hour;
    ptr[6] = c.minute;
    ptr[7] = c.second;
    toEndian<uint32_t>(c.nanosecond, ptr + 8, e);
    return CodecStatus::Ok();
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

inline std::string formatDecodedValue(const DecodedValue& dv, Type type = Type::Byte) {
    if (!dv.valid())
        return "null";
    if (type == Type::Bool)
        return dv.asInt64() != 0 ? "true" : "false";
    return std::visit(
        [&](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, int64_t>)
                return type == Type::Time ? formatTimeString(static_cast<int32_t>(v)) : std::to_string(v);
            else if constexpr (std::is_same_v<T, uint64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, float>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return std::to_string(v);
            else /* std::string */
                return "\"" + v + "\"";
        },
        dv);
}

// --- Main API ---

inline DecodedValue decodeScalar(
    Type type, const uint8_t* ptr, size_t buffer_size, int bit_index = 0, int count = 0, Endian e = Endian::Big) {
    auto check = [&](size_t needed) -> bool { return buffer_size >= needed; };
    switch (type) {
        case Type::Bool:
            if (!check(1))
                return {};
            return DecodedValue::makeBool(static_cast<bool>((ptr[0] >> bit_index) & 0x01));
        case Type::Byte:
        case Type::USInt:
        case Type::Char:
            if (!check(1))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(ptr[0]));
        case Type::SInt:
            if (!check(1))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(static_cast<int8_t>(ptr[0])));
        case Type::Int:
            if (!check(2))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int16_t>(ptr, e)));
        case Type::UInt:
        case Type::Word:
            if (!check(2))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint16_t>(ptr, e)));
        case Type::Date:
            if (!check(2))
                return {};
            return DecodedValue::makeString(Date(fromEndian<uint16_t>(ptr, e)).toString());
        case Type::WChar:
            if (!check(2))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint16_t>(ptr, e)));
        case Type::DInt:
            if (!check(4))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int32_t>(ptr, e)));
        case Type::DateTime: {
            if (!check(8))
                return {};
            DateTime dt;
            std::memcpy(dt.data, ptr, 8);
            return DecodedValue::makeString(dt.toString());
        }
        case Type::Time:
            if (!check(4))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int32_t>(ptr, e)));
        case Type::UDInt:
        case Type::DWord:
            if (!check(4))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint32_t>(ptr, e)));
        case Type::TimeOfDay:
            if (!check(4))
                return {};
            return DecodedValue::makeString(TOD(fromEndian<uint32_t>(ptr, e)).toString());
        case Type::LInt:
            if (!check(8))
                return {};
            return DecodedValue::makeSigned(static_cast<int64_t>(fromEndian<int64_t>(ptr, e)));
        case Type::ULInt:
        case Type::LWord:
            if (!check(8))
                return {};
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromEndian<uint64_t>(ptr, e)));
        case Type::LTime:
            if (!check(8))
                return {};
            return DecodedValue::makeString(LTime(fromEndian<int64_t>(ptr, e)).toString());
        case Type::LTimeOfDay:
            if (!check(8))
                return {};
            return DecodedValue::makeString(LTOD(fromEndian<uint64_t>(ptr, e)).toString());
        case Type::Real:
            if (!check(4))
                return {};
            return DecodedValue::makeFloat(fromEndian<float>(ptr, e));
        case Type::LReal:
            if (!check(8))
                return {};
            return DecodedValue::makeDouble(fromEndian<double>(ptr, e));
        case Type::DTL: {
            if (!check(12))
                return {};
            DTL dtl;
            dtl.year = fromEndian<uint16_t>(ptr, e);
            dtl.month = ptr[2];
            dtl.day = ptr[3];
            dtl.hour = ptr[5];
            dtl.minute = ptr[6];
            dtl.second = ptr[7];
            dtl.nanosecond = fromEndian<uint32_t>(ptr + 8, e);
            return DecodedValue::makeString(dtl.toString());
        }
        case Type::String: {
            if (!check(2))
                return {};
            uint8_t cur_len = ptr[1];
            int max_len = (count > 0 ? count : 254);
            if (cur_len > max_len)
                cur_len = static_cast<uint8_t>(max_len);
            if (!check(2 + cur_len))
                return {};
            return DecodedValue::makeString(std::string(reinterpret_cast<const char*>(&ptr[2]), cur_len));
        }
        case Type::WString: {
            if (!check(4))
                return {};
            int cur_len = static_cast<int>(fromEndian<uint16_t>(ptr + 2, e));
            int max_len = (count > 0 ? count : 16382);
            if (cur_len > max_len)
                cur_len = max_len;
            if (!check(4 + cur_len * 2))
                return {};
            std::string result;
            result.reserve(static_cast<std::size_t>(cur_len) * 3);
            for (int i = 0; i < cur_len; ++i) {
                uint16_t ch = fromEndian<uint16_t>(ptr + 4 + (i * 2), e);
                if (ch >= 0xD800u && ch <= 0xDBFFu && i + 1 < cur_len) {
                    uint16_t low = fromEndian<uint16_t>(ptr + 4 + ((i + 1) * 2), e);
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
            uint32_t cur_len = fromEndian<uint32_t>(ptr + 4, e);
            uint32_t max_len = (count > 0 ? static_cast<uint32_t>(count) : fromEndian<uint32_t>(ptr, e));
            if (cur_len > max_len)
                cur_len = max_len;
            if (!check(8 + cur_len))
                return {};
            return DecodedValue::makeString(std::string(reinterpret_cast<const char*>(&ptr[8]), cur_len));
        }
        case Type::XWString: {
            if (!check(8))
                return {};
            uint32_t cur_len = fromEndian<uint32_t>(ptr + 4, e);
            uint32_t max_len = (count > 0 ? static_cast<uint32_t>(count) : fromEndian<uint32_t>(ptr, e));
            if (cur_len > max_len)
                cur_len = max_len;
            if (!check(8 + cur_len * 2))
                return {};
            std::string result;
            result.reserve(static_cast<std::size_t>(cur_len) * 3);
            for (uint32_t i = 0; i < cur_len; ++i) {
                uint16_t ch = fromEndian<uint16_t>(ptr + 8 + (i * 2), e);
                if (ch >= 0xD800u && ch <= 0xDBFFu && i + 1 < cur_len) {
                    uint16_t low = fromEndian<uint16_t>(ptr + 8 + ((i + 1) * 2), e);
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
            return DecodedValue::makeUnsigned(static_cast<uint64_t>(fromBE<uint16_t>(ptr)));
        default:
            return {};
    }
}

inline CodecStatus encodeScalar(
    const DecodedValue& dv, Type type, uint8_t* ptr, size_t buffer_size, int bit_index = 0, int count = 0, Endian e = Endian::Big) {
    if (!dv.valid())
        return CodecStatus::Error("invalid value");
    switch (type) {
        case Type::Bool:
            return encodeBool(dv.asInt64() != 0, bit_index, ptr, buffer_size);
        case Type::Byte:
        case Type::USInt:
        case Type::Char:
            return encodeU8(static_cast<uint8_t>(dv.asInt64()), ptr, buffer_size);
        case Type::SInt:
            return encodeI8(static_cast<int8_t>(dv.asInt64()), ptr, buffer_size);
        case Type::Int:
            return encodeI16(static_cast<int16_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::UInt:
        case Type::Word:
        case Type::Counter:
        case Type::Timer:
            return encodeU16(static_cast<uint16_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::DInt:
        case Type::Time:
            return encodeI32(static_cast<int32_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::UDInt:
        case Type::DWord:
            return encodeU32(static_cast<uint32_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::LInt:
            return encodeI64(dv.asInt64(), ptr, buffer_size, e);
        case Type::ULInt:
        case Type::LWord:
            return encodeU64(static_cast<uint64_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::Real:
            return encodeReal(static_cast<float>(dv.asDouble()), ptr, buffer_size, e);
        case Type::LReal:
            return encodeLReal(dv.asDouble(), ptr, buffer_size, e);
            // AFTER
        case Type::String:
            return encodeString(dv.s().c_str(), static_cast<int>(dv.s().length()), count, ptr, buffer_size);
        case Type::XString:
            return encodeXString(dv.s().c_str(), static_cast<int>(dv.s().length()), count, ptr, buffer_size, e);
        case Type::WString: {
            std::u16string utf16;
            const std::string& input = dv.s();
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
                return CodecStatus::Error("invalid UTF-8 string for WSTRING");
            return encodeWString(
                reinterpret_cast<const uint16_t*>(utf16.c_str()), static_cast<int>(utf16.size()), count, ptr, buffer_size, e);
        }
        case Type::XWString: {
            std::u16string utf16;
            const std::string& input = dv.s();
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
                return CodecStatus::Error("invalid UTF-8 string for XWSTRING");
            return encodeXWString(
                reinterpret_cast<const uint16_t*>(utf16.c_str()), static_cast<int>(utf16.size()), count, ptr, buffer_size, e);
        }
        case Type::DateTime: {
            if (dv.kind() != ValueKind::String)
                return CodecStatus::Error("expected ISO string for DateTime");
            int y, m, d, h, min, s, ms;
            if (std::sscanf(dv.s().c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &m, &d, &h, &min, &s, &ms) < 6)
                return CodecStatus::Error("invalid DateTime string");
            return encodeDateTime(y, m, d, h, min, s, 0, ptr, buffer_size);
        }
        case Type::DTL: {
            if (dv.kind() != ValueKind::String)
                return CodecStatus::Error("expected ISO string for DTL");
            DtlComponents c;
            if (std::sscanf(dv.s().c_str(), "%hu-%hhu-%hhu %hhu:%hhu:%hhu.%u", &c.year, &c.month, &c.day, &c.hour, &c.minute, &c.second,
                    &c.nanosecond) < 6)
                return CodecStatus::Error("invalid DTL string");
            return encodeDtl(c, ptr, buffer_size, e);
        }
        case Type::Date: {
            if (dv.kind() == ValueKind::String) {
                struct tm ti{};
                if (std::sscanf(dv.s().c_str(), "%d-%d-%d", &ti.tm_year, &ti.tm_mon, &ti.tm_mday) == 3) {
                    ti.tm_year -= 1900;
                    ti.tm_mon -= 1;
                    return encodeU16(Date(ti).get(), ptr, buffer_size, e);
                }
                return CodecStatus::Error("invalid Date string");
            }
            return encodeU16(static_cast<uint16_t>(dv.asInt64()), ptr, buffer_size, e);
        }
        case Type::TimeOfDay:
            return encodeU32(static_cast<uint32_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::LTimeOfDay:
            return encodeU64(static_cast<uint64_t>(dv.asInt64()), ptr, buffer_size, e);
        case Type::LTime:
            return encodeI64(dv.asInt64(), ptr, buffer_size, e);
        default:
            return CodecStatus::Error("unsupported type for scalar encode");
    }
}

} // namespace s7codec
