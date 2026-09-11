#pragma once
// an Alias over s7codec::Type;
#include <fmt/core.h>
#include <array>
#include <open62541/types.h>
#include <optional>
#include <s7codec/codec.hpp>
#include <stdexcept>
namespace sgrn::scl
{
using DataType = s7codec::Type;

struct PlcTypeInfo {
    DataType type;
    const char* name;
    uint8_t storage_bytes; // 0 for String/WString/XString/XWString — dynamic
    int ua_type_index;     // -1 if no direct UA scalar mapping (uses UA_TYPES_...)
    int eip_type_index;    // placeholder for EtherNet/IP projection
    bool is_string;
    bool is_temporal;
};

#define IS_TEMPORAL true
#define IS_NOT_TEMPORAL false
#define IS_STRING true
#define IS_NOT_STRING false
#define NOT_PROJECTED_TO_EIP_YET -1
#define DYNAMIC 0

constexpr std::array<PlcTypeInfo, 32> kTypeTable = {{

    {DataType::Bool, "BOOL", 1, UA_TYPES_BOOLEAN, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::SInt, "SINT", 1, UA_TYPES_SBYTE, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::USInt, "USINT", 1, UA_TYPES_BYTE, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::Byte, "BYTE", 1, UA_TYPES_BYTE, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::Char, "CHAR", 1, UA_TYPES_BYTE, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::Int, "INT", 2, UA_TYPES_INT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::UInt, "UINT", 2, UA_TYPES_UINT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::Word, "WORD", 2, UA_TYPES_UINT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::DInt, "DINT", 4, UA_TYPES_INT32, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::UDInt, "UDINT", 4, UA_TYPES_UINT32, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::DWord, "DWORD", 4, UA_TYPES_UINT32, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::Real, "REAL", 4, UA_TYPES_FLOAT, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::LInt, "LINT", 8, UA_TYPES_INT64, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::ULInt, "ULINT", 8, UA_TYPES_UINT64, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::LWord, "LWORD", 8, UA_TYPES_UINT64, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::LReal, "LREAL", 8, UA_TYPES_DOUBLE, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::WChar, "WCHAR", 2, UA_TYPES_UINT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},
    {DataType::String, "STRING", DYNAMIC, UA_TYPES_STRING, NOT_PROJECTED_TO_EIP_YET, IS_STRING, IS_NOT_TEMPORAL},
    {DataType::WString, "WSTRING", DYNAMIC, UA_TYPES_STRING, NOT_PROJECTED_TO_EIP_YET, IS_STRING, IS_NOT_TEMPORAL},
    {DataType::XString, "XSTRING", DYNAMIC, UA_TYPES_STRING, NOT_PROJECTED_TO_EIP_YET, IS_STRING, IS_NOT_TEMPORAL},
    {DataType::XWString, "XWSTRING", DYNAMIC, UA_TYPES_STRING, NOT_PROJECTED_TO_EIP_YET, IS_STRING, IS_NOT_TEMPORAL},
    {DataType::Time, "TIME", 4, UA_TYPES_DOUBLE, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::LTime, "LTIME", 8, UA_TYPES_INT64, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::Date, "DATE", 2, UA_TYPES_UINT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::TimeOfDay, "TOD", 4, UA_TYPES_STRING, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::LTimeOfDay, "LTOD", 8, UA_TYPES_UINT64, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::DateTime, "DT", 8, UA_TYPES_DATETIME, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::DTL, "DTL", 12, UA_TYPES_DATETIME, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::LDT, "LDT", 8, UA_TYPES_DATETIME, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::LDTL, "LDTL", 8, UA_TYPES_DATETIME, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::Timer, "TIMER", 2, UA_TYPES_UINT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_TEMPORAL},
    {DataType::Counter, "COUNTER", 2, UA_TYPES_UINT16, NOT_PROJECTED_TO_EIP_YET, IS_NOT_STRING, IS_NOT_TEMPORAL},

}};

inline constexpr std::optional<PlcTypeInfo> getInfoOf(DataType t) {
    for (auto& e : kTypeTable) {
        if (e.type == t)
            return e;
    }
    return std::nullopt;
}

inline constexpr const PlcTypeInfo* info_of(DataType t) {
    for (auto& e : kTypeTable) {
        if (e.type == t)
            return &e;
    }
    return nullptr;
}

} // namespace sgrn::scl

template <>
struct fmt::formatter<sgrn::scl::DataType> : formatter<std::string_view> {
    auto format(sgrn::scl::DataType t_type, format_context& t_ctx) const {
        return formatter<std::string_view>::format(s7codec::s7TypeToString(t_type), t_ctx);
    }
};
#undef IS_TEMPORAL
#undef IS_NOT_TEMPORAL
#undef IS_NOT_STRING
#undef IS_STRING
#undef DYNAMIC
