#pragma once

#include <string_view>

namespace sgrn
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr const char kTypeBool[] = "Bool";
constexpr const char kTypeInt[] = "Int";
constexpr const char kTypeUInt[] = "UInt";
constexpr const char kTypeFloat[] = "Float";
constexpr const char kTypeString[] = "String";
constexpr const char kTypeDateTime[] = "DateTime";
constexpr const char kTypeStruct[] = "Struct";
constexpr const char kTypeRaw[] = "Raw";
constexpr const char kTypeUnkown[] = "Unknown";

/**
 * @brief Protocol-agnostic universal data types.
 */
enum class UniversalType { Bool, Int, UInt, Float, String, DateTime, Struct, Raw, Unknown };

inline std::string_view typeToString(UniversalType t_t) {
    switch (t_t) {
        case UniversalType::Bool:
            return kTypeBool;
        case UniversalType::Int:
            return kTypeInt;
        case UniversalType::UInt:
            return kTypeUInt;
        case UniversalType::Float:
            return kTypeFloat;
        case UniversalType::String:
            return kTypeString;
        case UniversalType::DateTime:
            return kTypeDateTime;
        case UniversalType::Struct:
            return kTypeStruct;
        case UniversalType::Raw:
            return kTypeRaw;
        default:
            return kTypeUnkown;
    }
}

inline UniversalType stringToType(std::string_view t_s) {
    if (t_s == kTypeBool)
        return UniversalType::Bool;
    if (t_s == kTypeInt)
        return UniversalType::Int;
    if (t_s == kTypeUInt)
        return UniversalType::UInt;
    if (t_s == kTypeFloat)
        return UniversalType::Float;
    if (t_s == kTypeString)
        return UniversalType::String;
    if (t_s == kTypeDateTime)
        return UniversalType::DateTime;
    if (t_s == kTypeStruct)
        return UniversalType::Struct;
    if (t_s == kTypeRaw)
        return UniversalType::Raw;
    return UniversalType::Unknown;
}

} // namespace sgrn
