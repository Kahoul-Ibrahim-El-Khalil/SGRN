#pragma once
// an Alias over s7codec::Type;
#include <fmt/core.h>
#include <array>
#include <open62541/types.h>
#include <s7codec/codec.hpp>
#include <stdexcept>

namespace sgrn::scl
{
using DataType = s7codec::Type;

struct PlcTypeInfo {
    DataType type;
    const char* name;
    int storage_bytes;  // 0 for String/WString/XString/XWString — dynamic
    int ua_type_index;  // -1 if no direct UA scalar mapping (uses UA_TYPES_...)
    int eip_type_index; // placeholder for EtherNet/IP projection
    bool is_string;
    bool is_temporal;
};

constexpr std::array<PlcTypeInfo, 32> kTypeTable = {{
    {DataType::Bool, "BOOL", 1, UA_TYPES_BOOLEAN, -1, false, false},
    {DataType::SInt, "SINT", 1, UA_TYPES_SBYTE, -1, false, false},
    {DataType::USInt, "USINT", 1, UA_TYPES_BYTE, -1, false, false},
    {DataType::Byte, "BYTE", 1, UA_TYPES_BYTE, -1, false, false},
    {DataType::Char, "CHAR", 1, UA_TYPES_BYTE, -1, false, false},
    {DataType::Int, "INT", 2, UA_TYPES_INT16, -1, false, false},
    {DataType::UInt, "UINT", 2, UA_TYPES_UINT16, -1, false, false},
    {DataType::Word, "WORD", 2, UA_TYPES_UINT16, -1, false, false},
    {DataType::DInt, "DINT", 4, UA_TYPES_INT32, -1, false, false},
    {DataType::UDInt, "UDINT", 4, UA_TYPES_UINT32, -1, false, false},
    {DataType::DWord, "DWORD", 4, UA_TYPES_UINT32, -1, false, false},
    {DataType::Real, "REAL", 4, UA_TYPES_FLOAT, -1, false, false},
    {DataType::LInt, "LINT", 8, UA_TYPES_INT64, -1, false, false},
    {DataType::ULInt, "ULINT", 8, UA_TYPES_UINT64, -1, false, false},
    {DataType::LWord, "LWORD", 8, UA_TYPES_UINT64, -1, false, false},
    {DataType::LReal, "LREAL", 8, UA_TYPES_DOUBLE, -1, false, false},
    {DataType::WChar, "WCHAR", 2, UA_TYPES_UINT16, -1, false, false},
    {DataType::String, "STRING", 0, UA_TYPES_STRING, -1, true, false},
    {DataType::WString, "WSTRING", 0, UA_TYPES_STRING, -1, true, false},
    {DataType::XString, "XSTRING", 0, UA_TYPES_STRING, -1, true, false},
    {DataType::XWString, "XWSTRING", 0, UA_TYPES_STRING, -1, true, false},
    {DataType::Time, "TIME", 4, UA_TYPES_DOUBLE, -1, false, true},
    {DataType::LTime, "LTIME", 8, UA_TYPES_INT64, -1, false, true},
    {DataType::Date, "DATE", 2, UA_TYPES_UINT16, -1, false, true},
    {DataType::TimeOfDay, "TOD", 4, UA_TYPES_STRING, -1, false, true},
    {DataType::LTimeOfDay, "LTOD", 8, UA_TYPES_UINT64, -1, false, true},
    {DataType::DateTime, "DT", 8, UA_TYPES_DATETIME, -1, false, true},
    {DataType::DTL, "DTL", 12, UA_TYPES_DATETIME, -1, false, true},
    {DataType::LDT, "LDT", 8, UA_TYPES_DATETIME, -1, false, true},
    {DataType::LDTL, "LDTL", 8, UA_TYPES_DATETIME, -1, false, true},
    {DataType::Timer, "TIMER", 2, UA_TYPES_UINT16, -1, false, true},
    {DataType::Counter, "COUNTER", 2, UA_TYPES_UINT16, -1, false, false},
}};

inline constexpr const PlcTypeInfo& info_of(DataType t) {
    for (auto& e : kTypeTable) {
        if (e.type == t)
            return e;
    }
    throw std::logic_error("Unmapped DataType in PlcTypeInfo table");
}

} // namespace sgrn::scl

template <>
struct fmt::formatter<sgrn::scl::DataType> : formatter<std::string_view> {
    auto format(sgrn::scl::DataType t_type, format_context& t_ctx) const {
        return formatter<std::string_view>::format(s7codec::s7TypeToString(t_type), t_ctx);
    }
};
