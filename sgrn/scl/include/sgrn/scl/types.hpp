#pragma once

#include <sgrn/Result.hpp>
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
#include <sgrn/scl/types/DataType.hpp>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/scl/types/DbSchema.hpp>
#include <sgrn/scl/types/Error.hpp>
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
// Error Handling — protocol-neutral structured error codes
// ---------------------------------------------------------------------------

/**
 * @brief Schema/parse/semantic error codes.
 *
 * Used when the failure is caused by bad data, type mismatches, structural
 * conflicts, or serialization problems — nothing to do with the file system
 * or the PLC wire protocol.
 */
enum class SchemaCode : int {
    Generic = 1,
    ParseError = 4,
    Conflict = 5,
    NotFound = 6,
    InvalidType = 9,
    OutOfRange = 10,
    SerializationError = 11,
    OptimizedAccess = 8, ///< S7 optimised-access block can't be decoded
};

/**
 * @brief File-system / IO error codes.
 *
 * Used when the failure is caused by missing files, unreadable paths,
 * or failed write operations — nothing to do with schema semantics or the
 * PLC wire protocol.
 */
enum class IoCode : int {
    FileNotFound = 2,
    IoError = 3,
};

struct Error {
    SchemaCode code_;
    std::string message_;

    SchemaCode code() const {
        return code_;
    }
    std::string string() const {
        return message_;
    }
};
struct IoError {
    IoCode code_;
    std::string message_;
    IoCode code() const {
        return code_;
    }
    std::string string() const {
        return message_;
    }
};
// ---------------------------------------------------------------------------
// Err — static factory for schema/semantic errors (one-line returns).
// Each method returns Error<Error>, implicitly convertible to any
// Result<T, Error>.
// ---------------------------------------------------------------------------
struct Err {
    template <typename... Args>
    static auto NotFound(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::NotFound, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto ParseError(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::ParseError, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto Conflict(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::Conflict, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto InvalidType(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::InvalidType, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto OutOfRange(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::OutOfRange, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto SerializationError(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::SerializationError, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto Generic(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::Generic, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto OptimizedAccess(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::OptimizedAccess, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }

    // ── Legacy aliases kept during migration ──────────────────────────────
    // These return unexpected<Error> (not IoError) so existing call sites
    // with return type Result<T, Error> continue to compile.
    // Migrate callers to IoErr::* for proper IoError typing.

    /// @deprecated Use IoErr::DeviceError or sgrn::gateway::io::S7ProtocolError instead.
    template <typename... Args>
    static auto DeviceError(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::Generic, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    /// @deprecated Use IoErr::IoError instead.
    template <typename... Args>
    static auto IoError(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::Generic, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    /// @deprecated Use IoErr::FileNotFound instead.
    template <typename... Args>
    static auto FileNotFound(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error(Error{SchemaCode::NotFound, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
};

// ---------------------------------------------------------------------------
// IoErr — static factory for file-system IO errors (one-line returns).
// Returns Error<IoError>, implicitly convertible to Result<T, IoError>.
// ---------------------------------------------------------------------------
struct IoErr {
    template <typename... Args>
    static auto FileNotFound(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error({IoCode::FileNotFound, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
    template <typename... Args>
    static auto IoError(fmt::format_string<Args...> t_f, Args&&... t_a) {
        return Error({IoCode::IoError, fmt::format(t_f, std::forward<Args>(t_a)...)});
    }
};

// ---------------------------------------------------------------------------
// Backward compatibility aliases (deprecated — remove after full migration)
// ---------------------------------------------------------------------------
/// @deprecated Use SchemaCode
using ErrorCode = SchemaCode;
/// @deprecated Use Error
using SchemaError = Error;
/// @deprecated Use SchemaCode
using S7ErrorCode = SchemaCode;
/// @deprecated Use Error
using S7Error = Error;
/// @deprecated Use DataType

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
    return s7codec::typeSpanBytes(t_type, t_count);
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

template <>
struct fmt::formatter<sgrn::scl::Error> : formatter<std::string_view> {
    auto format(const sgrn::scl::Error& t_error, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("Error{{message=\"{}\"}}", t_error.string()), t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::scl::IoError> : formatter<std::string_view> {
    auto format(const sgrn::scl::IoError& t_error, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("IoError{{message=\"{}\"}}", t_error.string()), t_ctx);
    }
};
