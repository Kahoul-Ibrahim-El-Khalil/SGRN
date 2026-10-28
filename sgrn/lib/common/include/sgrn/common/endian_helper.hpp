#pragma once

#include <sgrn/scl/types.hpp>
#include <cstdio>
#include <cstring>
#include <s7codec/endian.hpp>
#include <vector>

namespace sgrn::common
{

/**
 * @brief Endian conversion helpers for common data types
 */
namespace endian_helper
{

using DataType = sgrn::scl::DataType;

/**
 * @brief Generic load from buffer with endian conversion
 * Handles all primitive types with proper endianness
 */
template <typename T>
inline T loadFromBuffer(const uint8_t* tp_data, s7codec::Endian t_endianness) {
    return s7codec::fromEndian<T>(tp_data, t_endianness);
}

/**
 * @brief Generic store to buffer with endian conversion
 */
template <typename T>
inline void storeToBuffer(T t_value, uint8_t* tp_data, s7codec::Endian t_endianness) {
    s7codec::toEndian<T>(t_value, tp_data, t_endianness);
}

/**
 * @brief Load value from buffer based on data type
 * Handles all data types with a single call
 * @return The value converted to string representation
 */
inline std::string loadValue(const uint8_t* tp_buffer, DataType t_type, s7codec::Endian t_endianness, int t_bit_index = 0) {

    if (t_type == DataType::Bool) {
        const bool v = (tp_buffer[0] & (1 << t_bit_index)) != 0;
        return v ? "true" : "false";
    }

    switch (t_type) {
        case DataType::Byte:
        case DataType::USInt:
        case DataType::Char:
            return std::to_string(tp_buffer[0]);

        case DataType::SInt: {
            int8_t v;
            std::memcpy(&v, tp_buffer, 1);
            return std::to_string(v);
        }

        case DataType::Word:
        case DataType::UInt:
            return std::to_string(loadFromBuffer<uint16_t>(tp_buffer, t_endianness));

        case DataType::Int:
            return std::to_string(loadFromBuffer<int16_t>(tp_buffer, t_endianness));

        case DataType::DWord:
        case DataType::UDInt:
        case DataType::Time:
        case DataType::TimeOfDay:
            return std::to_string(loadFromBuffer<uint32_t>(tp_buffer, t_endianness));

        case DataType::DInt:
            return std::to_string(loadFromBuffer<int32_t>(tp_buffer, t_endianness));

        case DataType::Real: {
            float v = loadFromBuffer<float>(tp_buffer, t_endianness);
            if (std::isinf(v) || std::isnan(v))
                return "null";
            return std::to_string(v);
        }

        case DataType::LWord:
        case DataType::ULInt:
        case DataType::LTime:
        case DataType::LTimeOfDay:
            return std::to_string(loadFromBuffer<uint64_t>(tp_buffer, t_endianness));

        case DataType::LInt:
            return std::to_string(loadFromBuffer<int64_t>(tp_buffer, t_endianness));

        case DataType::LReal: {
            double v = loadFromBuffer<double>(tp_buffer, t_endianness);
            if (std::isinf(v) || std::isnan(v))
                return "null";
            return std::to_string(v);
        }

        case DataType::DTL: {
            // S7 DTL (12 bytes): Year u16 | Month | Day | Weekday | Hour |
            // Minute | Second | Nanoseconds u32. Renders the same ISO-ish
            // string the gateway twin uses ("2000-01-01 00:04:00.000000000").
            // Unset (all-zero / out-of-range) DTLs render as "null" so CSV
            // rows carry an empty column — downstream parsers drop those.
            const uint16_t year = loadFromBuffer<uint16_t>(tp_buffer, t_endianness);
            const uint8_t month = tp_buffer[2];
            const uint8_t hour = tp_buffer[5];
            const uint32_t nanos = loadFromBuffer<uint32_t>(tp_buffer + 8, t_endianness);
            if (year < 1 || year > 9999 || tp_buffer[3] < 1 || tp_buffer[3] > 31 || month < 1 || month > 12 || hour > 23 ||
                tp_buffer[6] > 59 || tp_buffer[7] > 59) {
                return "null";
            }
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%09u", static_cast<unsigned>(year), static_cast<unsigned>(month),
                static_cast<unsigned>(tp_buffer[3]), static_cast<unsigned>(hour), static_cast<unsigned>(tp_buffer[6]),
                static_cast<unsigned>(tp_buffer[7]), nanos);
            return std::string(buf);
        }

        default:
            return "null";
    }
}

/**
 * @brief Directly loads 12-byte DTL into Unix epoch nanoseconds without intermediate string allocations.
 */
inline int64_t loadDtlNanoseconds(const uint8_t* tp_buffer, s7codec::Endian t_endianness) {
    if (!tp_buffer)
        return 0;
    return s7codec::decodeDtlToNanoseconds(s7codec::ReadBuffer{tp_buffer, 12}, s7codec::DecodeOptions{.endian = t_endianness});
}

/**
 * @brief Directly loads 12-byte DTL into Unix epoch milliseconds without intermediate string allocations.
 */
inline int64_t loadDtlMilliseconds(const uint8_t* tp_buffer, s7codec::Endian t_endianness) {
    return loadDtlNanoseconds(tp_buffer, t_endianness) / 1'000'000LL;
}

/**
 * @brief Store value to buffer based on data type
 * Handles all data types with proper endianness
 */
inline void storeValue(uint8_t* tp_buffer, DataType t_type, const uint8_t* tp_source, s7codec::Endian t_endianness) {

    switch (t_type) {
        case DataType::Byte:
        case DataType::USInt:
        case DataType::Char:
            tp_buffer[0] = tp_source[0];
            break;

        case DataType::SInt:
            storeToBuffer<int8_t>(*reinterpret_cast<const int8_t*>(tp_source), tp_buffer, t_endianness);
            break;

        case DataType::Word:
        case DataType::UInt:
            storeToBuffer<uint16_t>(loadFromBuffer<uint16_t>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::Int:
            storeToBuffer<int16_t>(loadFromBuffer<int16_t>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::DWord:
        case DataType::UDInt:
        case DataType::Time:
        case DataType::TimeOfDay:
            storeToBuffer<uint32_t>(loadFromBuffer<uint32_t>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::DInt:
            storeToBuffer<int32_t>(loadFromBuffer<int32_t>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::Real:
            storeToBuffer<float>(loadFromBuffer<float>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::LWord:
        case DataType::ULInt:
        case DataType::LTime:
        case DataType::LTimeOfDay:
            storeToBuffer<uint64_t>(loadFromBuffer<uint64_t>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::LInt:
            storeToBuffer<int64_t>(loadFromBuffer<int64_t>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        case DataType::LReal:
            storeToBuffer<double>(loadFromBuffer<double>(tp_source, t_endianness), tp_buffer, t_endianness);
            break;

        default:
            break;
    }
}

/**
 * @brief Extract bit value from buffer
 */
inline bool extractBit(const uint8_t* tp_buffer, int t_bit_index) {
    return (tp_buffer[0] & (1 << t_bit_index)) != 0;
}

/**
 * @brief Set bit value in buffer
 */
inline void setBit(uint8_t* tp_buffer, int t_bit_index, bool t_value) {
    if (t_value) {
        tp_buffer[0] |= (1 << t_bit_index);
    } else {
        tp_buffer[0] &= ~(1 << t_bit_index);
    }
}

} // namespace endian_helper

} // namespace sgrn::common
