#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/errors.hpp>
#include <sgrn/utils/strings.hpp>
#include <s7codec/s7.hpp>

#include <fmt/format.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "S7K.hpp"
#include <sgrn/scl/errors.hpp>
#include <sgrn/scl/types/DataType.hpp>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/scl/types/DbSchema.hpp>
#include <sgrn/scl/types/ParseResult.hpp>
#include <sgrn/scl/types/UdtDefinition.hpp>
#include <sgrn/scl/types/modbus/ModbusArea.hpp>
#include <sgrn/scl/types/modbus/ModbusVirtualEntry.hpp>
#include <sgrn/scl/types/modbus/ModbusVirtualMap.hpp>
namespace sgrn::scl
{

/**
 * @brief Checks if a value fits within the range of a target type T.
 */
template <typename T, typename U>
constexpr bool isInRange(U t_value) {
    return s7codec::isInRange<T>(t_value);
}
// ---------------------------------------------------------------------------
// Error Handling — Error enum (mirrors gateway/adapters/opcua/errors.hpp)
// ---------------------------------------------------------------------------
// Single enum replaces Error/IoError structs + Err/IoErr factories.
// toString() overload provides human-readable message; no payload needed.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Data Types
// ---------------------------------------------------------------------------

// DataType is defined in s7codec/types.hpp and re-exported via
// the `using DataType = s7codec::Type;` declaration above.

/**
 * @brief Security policies for S7 nodes.
 */
enum class SecurityPolicy {
    Relaxed, ///< Allow all writes (standard Snap7 behavior)
    Strict   ///< Enforce IP-based ACLs for write operations
};

// ---------------------------------------------------------------------------
// PLC Address Shorthand  (I0.0, IB3, IW6, ID0, QB4, MW10, MD8, T5, C12, Z4)
// ---------------------------------------------------------------------------

struct PlcAddress {
    int area{S7AreaMK}; ///< S7AreaPE, S7AreaPA, S7AreaMK, S7AreaTM, S7AreaCT, S7AreaDB
    uint16_t db_number{0};
    int byte_offset{0};
    int bit_index{-1};      ///< -1 = no bit; 0-7 = specific bit position
    int word_len{S7WLByte}; ///< S7WLBit, S7WLByte, S7WLWord, S7WLDWord, S7WLTimer, S7WLCounter
    int byte_count{1};      ///< bytes to transfer (1, 2, 4, 8)
    std::string label;      ///< normalised display label
};

struct PlcDbRawAddr {
    uint16_t db_number;
    int offset;
};

/**
 * @brief A single symbolic tag loaded from a TIA Portal tag table XML export.
 *
 * This structure bridges the gap between TIA Portal names and physical S7 addresses.
 * It is used for semantic debugging in the s7shell.
 */
struct PlcTag {
    std::string name;              ///< Tag name, e.g. "A", "StartButton"
    std::string table_name;        ///< Source <Tagtable name="...">
    std::string type_str;          ///< Original XML type, e.g. "Bool", "Word"
    std::string remark;            ///< Optional remark
    PlcAddress addr;               ///< Resolved physical PLC address
    DataType type{DataType::Bool}; ///< Resolved S7 type
};

} // namespace sgrn::scl

namespace sgrn::scl
{

/**
 * @brief Parses a string into a DataType, returning std::optional for easy usage.
 * Handles SGRN-specific aliases for backward compatibility.
 */
inline std::optional<DataType> parseDataType(const std::string& t_val) {
    DataType t_result;
    if (s7codec::stringToType(t_val.c_str(), t_result))
        return t_result;
    return std::nullopt;
}

/// Backward compat alias
inline std::optional<DataType> parseS7Type(const std::string& t_val) {
    return parseDataType(t_val);
}

// ---------------------------------------------------------------------------
// S7 Offset and Area Helpers
// ---------------------------------------------------------------------------

struct S7Offset {
    int byte;
    int bit;
};

inline std::optional<S7Offset> parseS7Offset(const std::string& t_val) {
    const auto dot = t_val.find('.');
    if (dot == std::string::npos) {
        auto b = sgrn::utils::strings::parseInt(t_val);
        return b ? std::optional<S7Offset>(S7Offset{b.value(), 0}) : std::nullopt;
    }
    auto b = sgrn::utils::strings::parseInt(t_val.substr(0, dot));
    auto i = sgrn::utils::strings::parseInt(t_val.substr(dot + 1));
    if (!b || !i || i.value() < 0 || i.value() > 7) {
        return std::nullopt;
    }
    return S7Offset{b.value(), i.value()};
}

struct S7AreaRef {
    int area{S7AreaDB};
    uint16_t db_number{0};
};

inline std::optional<S7AreaRef> parseAreaRef(const std::string& t_tok) {
    if (t_tok.empty()) {
        return std::nullopt;
    }
    const std::string u = sgrn::utils::strings::toUpper(t_tok);
    if (u == "M" || u == "MK") {
        return S7AreaRef{S7AreaMK, 0};
    }
    if (u == "I" || u == "PE" || u == "E") {
        return S7AreaRef{S7AreaPE, 0};
    }
    if (u == "Q" || u == "PA" || u == "A") {
        return S7AreaRef{S7AreaPA, 0};
    }
    if (u == "CT" || u == "C" || u == "Z") {
        return S7AreaRef{S7AreaCT, 0};
    }
    if (u == "TM" || u == "T") {
        return S7AreaRef{S7AreaTM, 0};
    }
    if (u.substr(0, 2) == "DB") {
        auto t_v = sgrn::utils::strings::parseInt(u.substr(2));
        if (t_v && isInRange<uint16_t>(*t_v)) {
            return S7AreaRef{S7AreaDB, static_cast<uint16_t>(*t_v)};
        }
    }
    auto t_v = sgrn::utils::strings::parseInt(u);
    if (t_v && isInRange<uint16_t>(*t_v)) {
        return S7AreaRef{S7AreaDB, static_cast<uint16_t>(*t_v)};
    }
    return std::nullopt;
}

inline std::optional<int> parseArea(std::string t_val) {
    const std::string clean_val = sgrn::utils::strings::toUpper(sgrn::utils::strings::trim(std::move(t_val)));
    if (clean_val == "DB")
        return S7AreaDB;
    if (clean_val == "MK" || clean_val == "M")
        return S7AreaMK;
    if (clean_val == "PE" || clean_val == "I")
        return S7AreaPE;
    if (clean_val == "PA" || clean_val == "Q")
        return S7AreaPA;
    if (clean_val == "CT" || clean_val == "C")
        return S7AreaCT;
    if (clean_val == "TM" || clean_val == "T")
        return S7AreaTM;
    return std::nullopt;
}

enum class SrvArea : int { PE = 0, PA = 1, MK = 2, CT = 3, TM = 4, DB = 5 };

struct S7ServerEvent {
    int64_t timestamp{0};
    int sender{0};
    uint32_t code{0};
    uint16_t ret_code{0};
    uint16_t param1{0};
    uint16_t param2{0};
    uint16_t param3{0};
    uint16_t param4{0};
};

using S7Event = S7ServerEvent;
using S7DateTime = std::tm;

inline std::optional<uint16_t> parseConnectionType(std::string t_val) {
    const std::string clean_val = sgrn::utils::strings::toUpper(sgrn::utils::strings::trim(std::move(t_val)));
    if (clean_val.empty() || clean_val == "PG")
        return 1; // CONNTYPE_PG
    if (clean_val == "OP")
        return 2; // CONNTYPE_OP
    if (clean_val == "BASIC")
        return 3; // CONNTYPE_BASIC
    return std::nullopt;
}

inline std::optional<int> parseBlockType(std::string t_val) {
    const std::string clean_val = sgrn::utils::strings::toUpper(sgrn::utils::strings::trim(std::move(t_val)));
    if (clean_val == "OB")
        return 0x38; // Block_OB
    if (clean_val == "DB")
        return 0x41; // Block_DB
    if (clean_val == "SDB")
        return 0x42; // Block_SDB
    if (clean_val == "FC")
        return 0x43; // Block_FC
    if (clean_val == "SFC")
        return 0x44; // Block_SFC
    if (clean_val == "FB")
        return 0x45; // Block_FB
    if (clean_val == "SFB")
        return 0x46; // Block_SFB
    return sgrn::utils::strings::parseIntFlexible(std::move(clean_val));
}

// ---------------------------------------------------------------------------
// S7 Type sizing and inference — delegates to s7codec::typeSpanBytes
// ---------------------------------------------------------------------------

struct RawTypeSpec {
    DataType type;
    int count{0};
};

inline std::optional<RawTypeSpec> parseRawTypeSpec(std::string t_tok) {
    t_tok = sgrn::utils::strings::trim(std::move(t_tok));
    int count = 0;
    const auto lb = t_tok.find('['), rb = t_tok.find(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1) {
        auto cnt = sgrn::utils::strings::parseInt(t_tok.substr(lb + 1, rb - lb - 1));
        if (!cnt)
            return std::nullopt;
        count = *cnt;
        t_tok = sgrn::utils::strings::trim(t_tok.substr(0, lb));
    }
    auto t_type = parseS7Type(t_tok);
    if (!t_type)
        return std::nullopt;
    if ((*t_type == DataType::String || *t_type == DataType::WString) && count <= 0)
        return std::nullopt;
    return RawTypeSpec{*t_type, count};
}

inline int rawTypeSpanBytes(DataType t_type, int t_count) {
    return s7codec::typeSpanBytes(t_type, t_count).value_or(0);
}

inline int rawTypeSpanBytes(const RawTypeSpec& t_spec) {
    return rawTypeSpanBytes(t_spec.type, t_spec.count);
}

inline std::optional<DataType> inferRawType(const rapidjson::Value& t_v) {
    if (t_v.IsBool()) {
        return DataType::Bool;
    }
    if (t_v.IsString()) {
        return DataType::String;
    }
    if (t_v.IsArray() && t_v.Size() > 0) {
        return inferRawType(t_v[0]);
    }
    if (t_v.IsDouble() && !t_v.IsInt64() && !t_v.IsUint64()) {
        return DataType::Real;
    }
    if (t_v.IsInt64()) {
        const auto s = t_v.GetInt64();
        if (s < 0) {
            if (s >= -128)
                return DataType::SInt;
            if (s >= -32768)
                return DataType::Int;
            if (s >= -2147483648LL)
                return DataType::DInt;
            return DataType::LInt;
        }
        const auto u = static_cast<uint64_t>(s);
        if (u <= 0xFF)
            return DataType::Byte;
        if (u <= 0xFFFF)
            return DataType::UInt;
        if (u <= 0xFFFFFFFF)
            return DataType::UDInt;
        return DataType::ULInt;
    }
    if (t_v.IsUint64()) {
        const auto u = t_v.GetUint64();
        if (u <= 0xFF)
            return DataType::Byte;
        if (u <= 0xFFFF)
            return DataType::UInt;
        if (u <= 0xFFFFFFFF)
            return DataType::UDInt;
        return DataType::ULInt;
    }
    return std::nullopt;
}
class PlcSchemaStore;
struct FieldTarget;

} // namespace sgrn::scl
