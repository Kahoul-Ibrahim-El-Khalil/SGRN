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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ── S7 protocol constants (self-contained, no snap7.h required) ─────────────
// When building inside the SGRN monorepo with snap7 available, the build system
// defines SGRN_HAS_SNAP7 and snap7.h provides these symbols.
// When building standalone (without snap7), we define them ourselves.
#ifdef SGRN_HAS_SNAP7
#include <snap7.h>
#else
using byte = unsigned char;
inline constexpr byte S7AreaPE = 0x81; // Process inputs  (I/E)
inline constexpr byte S7AreaPA = 0x82; // Process outputs (Q/A)
inline constexpr byte S7AreaMK = 0x83; // Merkers         (M)
inline constexpr byte S7AreaDB = 0x84; // Data blocks     (DB)
inline constexpr byte S7AreaCT = 0x1C; // Counters        (C/Z)
inline constexpr byte S7AreaTM = 0x1D; // Timers          (T)

inline constexpr int S7WLBit = 0x01;
inline constexpr int S7WLByte = 0x02;
inline constexpr int S7WLWord = 0x04;
inline constexpr int S7WLDWord = 0x06;
inline constexpr int S7WLReal = 0x08;
inline constexpr int S7WLCounter = 0x1C;
inline constexpr int S7WLTimer = 0x1D;
#endif

namespace sgrn::scl
{
// ---------------------------------------------------------------------------
// Re-export the canonical data type from the freestanding s7codec/ library.
// All downstream code should use sgrn::scl::DataType (or the legacy S7Type alias).
// ---------------------------------------------------------------------------
using DataType = s7codec::Type;

/**
 * @brief Checks if a value fits within the range of a target type T.
 */
template <typename T, typename U>
constexpr bool isInRange(U t_value) {
    return s7codec::isInRange<T>(t_value);
}

// ---------------------------------------------------------------------------
// String constants — type names delegate to s7codec::detail::
// ---------------------------------------------------------------------------

// Area aliases
constexpr const char str_area_m[] = "M";
constexpr const char str_area_mk[] = "MK";
constexpr const char str_area_i[] = "I";
constexpr const char str_area_pe[] = "PE";
constexpr const char str_area_e[] = "E";
constexpr const char str_area_q[] = "Q";
constexpr const char str_area_pa[] = "PA";
constexpr const char str_area_a[] = "A";
constexpr const char str_area_ct[] = "CT";
constexpr const char str_area_c[] = "C";
constexpr const char str_area_z[] = "Z";
constexpr const char str_area_tm[] = "TM";
constexpr const char str_area_t[] = "T";
constexpr const char str_area_db[] = "DB";

// Connection types
constexpr const char str_conn_pg[] = "PG";
constexpr const char str_conn_op[] = "OP";
constexpr const char str_conn_basic[] = "BASIC";

// Block types
constexpr const char str_block_ob[] = "OB";
constexpr const char str_block_db[] = "DB";
constexpr const char str_block_sdb[] = "SDB";
constexpr const char str_block_fc[] = "FC";
constexpr const char str_block_sfc[] = "SFC";
constexpr const char str_block_fb[] = "FB";
constexpr const char str_block_sfb[] = "SFB";

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
using Type = DataType;
/// @deprecated Use sgrn::Result<T, Error>
template <typename T>
using S7Result = ::sgrn::Result<T, Error>;

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
    if (u == str_area_m || u == str_area_mk) {
        return S7AreaRef{S7AreaMK, 0};
    }
    if (u == str_area_i || u == str_area_pe || u == str_area_e) {
        return S7AreaRef{S7AreaPE, 0};
    }
    if (u == str_area_q || u == str_area_pa || u == str_area_a) {
        return S7AreaRef{S7AreaPA, 0};
    }
    if (u == str_area_ct || u == str_area_c || u == str_area_z) {
        return S7AreaRef{S7AreaCT, 0};
    }
    if (u == str_area_tm || u == str_area_t) {
        return S7AreaRef{S7AreaTM, 0};
    }
    if (u.substr(0, 2) == str_area_db) {
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
    if (clean_val == str_area_db)
        return S7AreaDB;
    if (clean_val == str_area_mk || clean_val == str_area_m)
        return S7AreaMK;
    if (clean_val == str_area_pe || clean_val == str_area_i)
        return S7AreaPE;
    if (clean_val == str_area_pa || clean_val == str_area_q)
        return S7AreaPA;
    if (clean_val == str_area_ct || clean_val == str_area_c)
        return S7AreaCT;
    if (clean_val == str_area_tm || clean_val == str_area_t)
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
    if (clean_val.empty() || clean_val == str_conn_pg)
        return 1; // CONNTYPE_PG
    if (clean_val == str_conn_op)
        return 2; // CONNTYPE_OP
    if (clean_val == str_conn_basic)
        return 3; // CONNTYPE_BASIC
    return std::nullopt;
}

inline std::optional<int> parseBlockType(std::string t_val) {
    const std::string clean_val = sgrn::utils::strings::toUpper(sgrn::utils::strings::trim(std::move(t_val)));
    if (clean_val == str_block_ob)
        return 0x38; // Block_OB
    if (clean_val == str_block_db)
        return 0x41; // Block_DB
    if (clean_val == str_block_sdb)
        return 0x42; // Block_SDB
    if (clean_val == str_block_fc)
        return 0x43; // Block_FC
    if (clean_val == str_block_sfc)
        return 0x44; // Block_SFC
    if (clean_val == str_block_fb)
        return 0x45; // Block_FB
    if (clean_val == str_block_sfb)
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
    int t_count = 0;
    const auto lb = t_tok.find('['), rb = t_tok.find(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb + 1) {
        auto cnt = sgrn::utils::strings::parseInt(t_tok.substr(lb + 1, rb - lb - 1));
        if (!cnt)
            return std::nullopt;
        t_count = *cnt;
        t_tok = sgrn::utils::strings::trim(t_tok.substr(0, lb));
    }
    auto t_type = parseS7Type(t_tok);
    if (!t_type)
        return std::nullopt;
    if ((*t_type == DataType::String || *t_type == DataType::WString) && t_count <= 0)
        return std::nullopt;
    return RawTypeSpec{*t_type, t_count};
}

inline int rawTypeSpanBytes(DataType t_type, int t_count) {
    return s7codec::typeSpanBytes(t_type, t_count);
}

inline int rawTypeSpanBytes(const RawTypeSpec& t_spec) {
    return rawTypeSpanBytes(t_spec.type, t_spec.count);
}

inline std::optional<DataType> inferRawType(const rapidjson::Value& t_v) {
    if (t_v.IsBool())
        return DataType::Bool;
    if (t_v.IsString())
        return DataType::String;
    if (t_v.IsArray() && t_v.Size() > 0)
        return inferRawType(t_v[0]);
    if (t_v.IsDouble() && !t_v.IsInt64() && !t_v.IsUint64())
        return DataType::Real;
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

// ---------------------------------------------------------------------------
// Field and UDT definitions
// ---------------------------------------------------------------------------

struct DbField {
    std::string name;
    int offset{0};
    int bit_index{0};
    DataType type{DataType::Byte};
    int count{0};

    std::string udt_name;
    std::vector<DbField> children;
    int struct_size{0};
    s7codec::Endian endianness{s7codec::Endian::Big};
    std::optional<std::string> unit;
    bool trigger_events{false};
    bool is_dynamic{false};
};

struct UdtDefinition {
    uint16_t udt_number{0};
    std::string name;
    int size_bytes{0};
    int max_depth{0};
    std::vector<DbField> fields;
    s7codec::Endian endianness{s7codec::Endian::Big};
    bool trigger_events{false};
};

// ---------------------------------------------------------------------------
// Data Block registry
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Modbus exposure area — set by #MODBUS_* block-level directives.
// None = DB is not exposed via Modbus (default).
// ---------------------------------------------------------------------------
enum class ModbusArea : uint8_t {
    None = 0,
    Holding = 1,  ///< 4x — holding registers (FC03 read / FC16 write), read-write
    Input = 2,    ///< 3x — input registers   (FC04 read-only)
    Coil = 3,     ///< 0x — coils             (FC01 read / FC15 write), read-write bits
    Discrete = 4, ///< 1x — discrete inputs   (FC02 read-only bits)
};

struct DbSchema {
    uint16_t db_number{0};
    std::string db_name;
    int size_bytes{0};
    int max_depth{0};
    std::vector<DbField> fields;

    std::string source_file;
    s7codec::Endian endianness{s7codec::Endian::Big};
    bool trigger_events{false};
    ModbusArea modbus_area{ModbusArea::None}; ///< Modbus exposure directive
};

using DataBlockRegistry = DbSchema;
using DbRawBuffer = std::vector<uint8_t>;
using DataBlockRawBuffer = DbRawBuffer;
using DbData = std::string;
using DataBlockData = DbData;

struct ParseResult {
    std::vector<DbSchema> dbs;
    std::vector<UdtDefinition> udts;
    std::vector<std::string> warnings;
};

} // namespace sgrn::scl

// ---------------------------------------------------------------------------
// fmt::formatter specializations
// ---------------------------------------------------------------------------

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

template <>
struct fmt::formatter<sgrn::scl::DataType> : formatter<std::string_view> {
    auto format(sgrn::scl::DataType t_type, format_context& t_ctx) const {
        return formatter<std::string_view>::format(s7codec::s7TypeToString(t_type), t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::scl::DbField> : formatter<std::string_view> {
    auto format(const sgrn::scl::DbField& t_field, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format(
                "DbField{{name=\"{}\", offset={}, bit_index={}, type={}, count={}, udt=\"{}\", struct_size={}, children={}, unit=\"{}\"}}",
                t_field.name, t_field.offset, t_field.bit_index, t_field.type, t_field.count, t_field.udt_name, t_field.struct_size,
                t_field.children.size(), t_field.unit.value_or("")),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::scl::UdtDefinition> : formatter<std::string_view> {
    auto format(const sgrn::scl::UdtDefinition& t_udt, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("UdtDefinition{{udt_number={}, name=\"{}\", size_bytes={}, fields={}}}",
                                                       t_udt.udt_number, t_udt.name, t_udt.size_bytes, t_udt.fields.size()),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::scl::DbSchema> : formatter<std::string_view> {
    auto format(const sgrn::scl::DbSchema& t_db, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("DbSchema{{db_number={}, db_name=\"{}\", size_bytes={}, fields={}, source_file=\"{}\"}}", t_db.db_number,
                t_db.db_name, t_db.size_bytes, t_db.fields.size(), t_db.source_file),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::scl::ParseResult> : formatter<std::string_view> {
    auto format(const sgrn::scl::ParseResult& t_result, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("ParseResult{{dbs={}, udts={}}}", t_result.dbs.size(), t_result.udts.size()), t_ctx);
    }
};

namespace sgrn::scl
{
class PlcSchemaStore;
struct FieldTarget;
} // namespace sgrn::scl
