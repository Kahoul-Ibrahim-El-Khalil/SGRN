#include <fmt/format.h>
#include <sgrn/scl/types.hpp>
#include <sgrn/scl/utils.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/endianess.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::scl
{
using sgrn::utils::strings::trim;

int fieldElementSpanBytes(const DbField& f) {
    switch (kind_of(f)) {
        case FieldKind::String:
            switch (f.type) {
                case DataType::String:
                    return 2 + f.string_capacity;
                case DataType::WString:
                    return 4 + f.string_capacity * 2;
                case DataType::XString:
                    return 8 + f.string_capacity;
                case DataType::XWString:
                    return 8 + f.string_capacity * 2;
                default:
                    return f.string_capacity;
            }
        case FieldKind::Struct:
            return std::max(1, f.struct_size);
        case FieldKind::Scalar:
        case FieldKind::Enum:
            return std::max(1, info_of(f.type).storage_bytes);
    }
    return 1;
}

int fieldSpanSize(const DbField& f) {
    const int elem = fieldElementSpanBytes(f);
    int base = f.count <= 1 ? elem : elem * f.count;
    return f.is_dynamic ? base + 4 : base;
}

// -----------------------------------------------------------------------------
// Parsing Utilities
// -----------------------------------------------------------------------------

/**
 * @brief Finds a field by its name in a Data Block registry.
 */
const DbField* findFieldByName(const DbSchema& t_reg, const std::string& t_name) {
    std::vector<DbField>::const_iterator it =
        std::find_if(t_reg.fields.begin(), t_reg.fields.end(), [&](const DbField& t_f) { return t_f.name == t_name; });
    return it == t_reg.fields.end() ? nullptr : &*it;
}

// -----------------------------------------------------------------------------
// Time Utilities
// -----------------------------------------------------------------------------

/**
 * @brief Parses an S7 Time string in format "#T19:10:10:100" or "[-]HH:MM:SS.mmm".
 * Returns total milliseconds.
 */
std::optional<int64_t> parseS7Time(const std::string& t_raw) {
    std::string s = sgrn::utils::strings::trim(t_raw);
    if (s.empty())
        return std::nullopt;

    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        s = s.substr(1);
    }

    // Handle #T prefix
    if (s.size() > 2 && s[0] == '#' && (s[1] == 'T' || s[1] == 't')) {
        s = s.substr(2);
    }

    unsigned h = 0, m = 0, sec = 0, ms = 0;
    // Try HH:MM:SS.mmm
    constexpr std::string_view kFmtS7Time = "%u:%u:%u.%u";
    constexpr std::string_view kFmtS7TimeAlt = "%u:%u:%u:%u";
    if (std::sscanf(s.c_str(), kFmtS7Time.data(), &h, &m, &sec, &ms) >= 3) {
        // success
    } else if (std::sscanf(s.c_str(), kFmtS7TimeAlt.data(), &h, &m, &sec, &ms) >= 3) {
        // success with colon for ms (some TIA formats)
    } else {
        return std::nullopt;
    }

    const int64_t abs_ms =
        static_cast<int64_t>(h) * 3600000 + static_cast<int64_t>(m) * 60000 + static_cast<int64_t>(sec) * 1000 + static_cast<int64_t>(ms);
    return negative ? -abs_ms : abs_ms;
}

std::optional<LocatedField> findFieldByPath(const std::vector<DbField>& t_fields, const std::string& t_path, int t_base_offset) {
    // Use string_view to avoid substr allocations at each recursion level
    auto impl = [](const std::vector<DbField>& t_flds, std::string_view t_p, int t_off, auto& t_self_ref) -> std::optional<LocatedField> {
        size_t sep = t_p.find_first_of("./");
        std::string_view current_name = (sep == std::string_view::npos) ? t_p : t_p.substr(0, sep);
        std::string_view remaining = (sep == std::string_view::npos) ? std::string_view{} : t_p.substr(sep + 1);

        std::string_view field_name = current_name;
        int array_index = 0;
        bool is_array_access = false;
        size_t bracket_open = current_name.find('[');
        if (bracket_open != std::string_view::npos) {
            size_t bracket_close = current_name.find(']', bracket_open);
            if (bracket_close != std::string_view::npos) {
                field_name = current_name.substr(0, bracket_open);
                std::string idx_str(current_name.substr(bracket_open + 1, bracket_close - bracket_open - 1));
                try {
                    array_index = std::stoi(idx_str);
                    is_array_access = true;
                } catch (...) {
                }
            }
        }

        auto it = std::find_if(t_flds.begin(), t_flds.end(), [&](const auto& t_f) { return t_f.name == field_name; });
        if (it == t_flds.end())
            return std::nullopt;

        int elem_size = 0;
        int result_bit_index = it->bit_index;
        bool is_bool_array = is_array_access && it->type == DataType::Bool;

        if (is_array_access && !is_bool_array) {
            elem_size = fieldElementSpanBytes(*it);
        }

        int abs_offset = t_off + it->offset;
        if (is_array_access) {
            int norm_idx = array_index - it->array_lower_bound;
            if (is_bool_array) {
                abs_offset += norm_idx / 8;
                result_bit_index = norm_idx % 8;
            } else {
                abs_offset += norm_idx * elem_size;
            }
        }

        if (remaining.empty()) {
            return LocatedField{&*it, abs_offset, result_bit_index};
        }
        return t_self_ref(it->children, remaining, abs_offset, t_self_ref);
    };
    return impl(t_fields, std::string_view(t_path), t_base_offset, impl);
}

// forward declarations of internal helpers
sgrn::Result<void, ::sgrn::scl::Error> encodeArrayValue(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e);
void serializeFieldToWriter(
    rapidjson::Writer<rapidjson::StringBuffer>& t_writer, const DbField& t_field, const uint8_t* tp_ptr, size_t t_buffer_size, int t_depth);

sgrn::Result<void, ::sgrn::scl::Error> applyJsonPatchToFields(
    const std::vector<DbField>& t_fields, const std::string& t_patch_json, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e) {
    rapidjson::Document patch;
    if (patch.Parse(t_patch_json.c_str()).HasParseError()) {
        return Error{SchemaCode::ParseError, "patch value must be a valid JSON string"};
    }
    if (!patch.IsObject()) {
        return Error{SchemaCode::Generic, "patch value must be a JSON object"};
    }

    for (const auto& t_field : t_fields) {
        if (!patch.HasMember(t_field.name.c_str()))
            continue;
        if (static_cast<size_t>(t_field.offset) >= t_buffer_size)
            continue;

        const rapidjson::Value& val = patch[t_field.name.c_str()];
        uint8_t* field_ptr = tp_ptr + t_field.offset;
        size_t field_buffer_remaining = t_buffer_size - t_field.offset;

        if (t_field.type == DataType::Struct && t_field.count <= 1 && val.IsObject()) {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
            val.Accept(t_writer);
            sgrn::Result<void, ::sgrn::scl::Error> status =
                applyJsonPatchToFields(t_field.children, sb.GetString(), field_ptr, field_buffer_remaining, t_e);
            if (!status.has_value())
                return status;
        } else {
            sgrn::Result<void, ::sgrn::scl::Error> status = encodeFieldRapidJson(t_field, val, field_ptr, field_buffer_remaining, 0, t_e);
            if (!status.has_value())
                return status;
        }
    }
    return {};
}

std::string parseSemanticValue(const std::string& t_raw) {
    const std::string val = sgrn::utils::strings::trim(t_raw);
    if (val.empty())
        return "\"\"";
    constexpr std::string_view kTrueLower = "true";
    constexpr std::string_view kTrueUpper = "TRUE";
    constexpr std::string_view kFalseLower = "false";
    constexpr std::string_view kFalseUpper = "FALSE";
    if (val == kTrueLower || val == kTrueUpper)
        return "true";
    if (val == kFalseLower || val == kFalseUpper)
        return "false";

    constexpr std::string_view kBinPrefix = "bin:";
    if (val.size() > kBinPrefix.size() && val.substr(0, kBinPrefix.size()) == kBinPrefix) {
        uint64_t t_v = 0;
        for (size_t i = kBinPrefix.size(); i < val.size(); ++i) {
            if (val[i] == '0' || val[i] == '1')
                t_v = (t_v << 1) | (val[i] - '0');
        }
        return std::to_string(t_v);
    }
    constexpr std::string_view kHexPrefix = "hex:";
    if (val.size() > kHexPrefix.size() && val.substr(0, kHexPrefix.size()) == kHexPrefix) {
        if (auto num = sgrn::utils::strings::parseUInt64(val.substr(kHexPrefix.size()), 16))
            return std::to_string(*num);
    }
    if (val.size() > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
        if (auto num = sgrn::utils::strings::parseUInt64(val, 16))
            return std::to_string(*num);
    }
    constexpr std::string_view kS7TimePrefix = "#T";
    constexpr std::string_view kS7TimePrefixLower = "#t";
    if (val.size() > kS7TimePrefix.size() &&
        (val.substr(0, kS7TimePrefix.size()) == kS7TimePrefix || val.substr(0, kS7TimePrefixLower.size()) == kS7TimePrefixLower)) {
        if (parseS7Time(val))
            return "\"" + val + "\"";
    }
    if (val.front() == '"' && val.back() == '"' && val.size() >= 2)
        return val;

    if (val.find('.') != std::string::npos) {
        if (sgrn::utils::strings::parseDouble(val))
            return val;
    } else {
        if (sgrn::utils::strings::parseInt64(val))
            return val;
    }
    return "\"" + val + "\"";
}

std::string parseRawValuePayload(const std::string& t_raw) {
    const std::string trimmed = sgrn::utils::strings::trim(t_raw);
    if (!trimmed.empty()) {
        rapidjson::Document doc;
        if (!doc.Parse(trimmed.c_str()).HasParseError())
            return trimmed;
    }
    if (trimmed.find(',') != std::string::npos) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
        t_writer.StartArray();
        std::istringstream in(trimmed);
        std::string part;
        while (std::getline(in, part, ',')) {
            std::string sem = parseSemanticValue(part);
            rapidjson::Document doc;
            doc.Parse(sem.c_str());
            doc.Accept(t_writer);
        }
        t_writer.EndArray();
        return sb.GetString();
    }
    return parseSemanticValue(trimmed);
}

sgrn::Result<void, ::sgrn::scl::Error> encodeDtlValue(
    const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e) {
    using namespace s7codec;
    DtlComponents dtl;
    if (t_value.IsObject()) {
        dtl.year = t_value.HasMember("year") && t_value["year"].IsUint() ? static_cast<uint16_t>(t_value["year"].GetUint()) : 2000;
        dtl.month = t_value.HasMember("month") && t_value["month"].IsUint() ? static_cast<uint8_t>(t_value["month"].GetUint()) : 1;
        dtl.day = t_value.HasMember("day") && t_value["day"].IsUint() ? static_cast<uint8_t>(t_value["day"].GetUint()) : 1;
        dtl.hour = t_value.HasMember("hour") && t_value["hour"].IsUint() ? static_cast<uint8_t>(t_value["hour"].GetUint()) : 0;
        dtl.minute = t_value.HasMember("minute") && t_value["minute"].IsUint() ? static_cast<uint8_t>(t_value["minute"].GetUint()) : 0;
        dtl.second = t_value.HasMember("second") && t_value["second"].IsUint() ? static_cast<uint8_t>(t_value["second"].GetUint()) : 0;
        dtl.nanosecond =
            t_value.HasMember("nanosecond") && t_value["nanosecond"].IsUint() ? static_cast<uint32_t>(t_value["nanosecond"].GetUint()) : 0;
    } else if (t_value.IsString()) {
        std::string s = t_value.GetString();
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, ns = 0;
        bool parsed = false;

        // Try parsing various formats:
        // 1. ISO-8601 with milliseconds: YYYY-MM-DDTHH:MM:SS.nnn
        if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%d", &year, &month, &day, &hour, &minute, &second, &ns) == 7) {
            parsed = true;
        }
        // 2. ISO-8601 without milliseconds: YYYY-MM-DDTHH:MM:SS
        else if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
            parsed = true;
        }
        // 3. Space separator with milliseconds: YYYY-MM-DD HH:MM:SS.nnn
        else if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &year, &month, &day, &hour, &minute, &second, &ns) == 7) {
            parsed = true;
        }
        // 4. Space separator without milliseconds: YYYY-MM-DD HH:MM:SS
        else if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
            parsed = true;
        }
        // 5. Date only: YYYY-MM-DD
        else if (std::sscanf(s.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
            parsed = true;
        }

        if (parsed) {
            dtl.year = static_cast<uint16_t>(year);
            dtl.month = static_cast<uint8_t>(month);
            dtl.day = static_cast<uint8_t>(day);
            dtl.hour = static_cast<uint8_t>(hour);
            dtl.minute = static_cast<uint8_t>(minute);
            dtl.second = static_cast<uint8_t>(second);

            if (ns > 0) {
                size_t dot_pos = s.find('.');
                if (dot_pos != std::string::npos) {
                    std::string frac = s.substr(dot_pos + 1);
                    while (frac.size() < 9)
                        frac += '0';
                    if (frac.size() > 9)
                        frac = frac.substr(0, 9);
                    try {
                        ns = std::stoi(frac);
                    } catch (...) {
                    }
                }
            }
            dtl.nanosecond = static_cast<uint32_t>(ns);

            // Compute weekday (Siemens: 1=Sunday … 7=Saturday)
            std::tm tm{};
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = minute;
            tm.tm_sec = second;
            tm.tm_isdst = -1;
            std::mktime(&tm); // fills tm_wday (0=Sunday)
            dtl.day_of_week = static_cast<uint8_t>(tm.tm_wday + 1);
        } else {
            return Error{SchemaCode::Generic, "invalid DTL format"};
        }
    } else {
        return Error{SchemaCode::Generic, "expected object or string for DTL"};
    }
    auto status = encodeDtl(dtl, tp_ptr, t_buffer_size, t_e);
    if (!status.has_value())
        return Error{SchemaCode::Generic, toString(status.error())};
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> encodeScalarValue(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, s7codec::Endian t_e) {
    using namespace s7codec;
    auto to_unsigned = [](const rapidjson::Value& t_v) -> std::optional<uint64_t> {
        if (t_v.IsUint64())
            return t_v.GetUint64();
        if (t_v.IsInt64() && t_v.GetInt64() >= 0)
            return static_cast<uint64_t>(t_v.GetInt64());
        if (t_v.IsString()) {
            std::string s = t_v.GetString();
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                return sgrn::utils::strings::parseUInt64(s, 16);
            return sgrn::utils::strings::parseUInt64(s);
        }
        return std::nullopt;
    };
    auto to_signed = [](const rapidjson::Value& t_v) -> std::optional<int64_t> {
        if (t_v.IsInt64())
            return t_v.GetInt64();
        if (t_v.IsUint64() && t_v.GetUint64() <= static_cast<uint64_t>(INT64_MAX))
            return static_cast<int64_t>(t_v.GetUint64());
        if (t_v.IsString())
            return sgrn::utils::strings::parseInt64(t_v.GetString());
        return std::nullopt;
    };
    auto to_float = [](const rapidjson::Value& t_v) -> std::optional<double> {
        if (t_v.IsNumber())
            return t_v.GetDouble();
        if (t_v.IsString())
            return sgrn::utils::strings::parseDouble(t_v.GetString());
        return std::nullopt;
    };

    switch (t_field.type) {
        case DataType::Bool: {
            bool b = false;
            if (t_value.IsBool()) {
                b = t_value.GetBool();
            } else if (t_value.IsInt()) {
                b = t_value.GetInt() != 0;
            } else if (t_value.IsUint()) {
                b = t_value.GetUint() != 0;
            } else if (t_value.IsString()) {
                std::string s = t_value.GetString();
                b = (s == "true" || s == "1" || s == "TRUE");
            } else if (t_value.IsNull()) {
                b = false;
            } else {
                return Error{SchemaCode::Generic, "expected boolean, integer, or string for Bool field"};
            }
            auto status = s7codec::encodeBool(b, t_field.bit_index, tp_ptr, t_buffer_size);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::Byte:
        case DataType::USInt:
        case DataType::Char: {
            auto t_v = to_unsigned(t_value);
            if (!t_v || !isInRange<uint8_t>(*t_v))
                return Error{SchemaCode::Generic, "Value out of range"};
            auto status = encodeU8(static_cast<uint8_t>(*t_v), tp_ptr, t_buffer_size);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::SInt: {
            auto t_v = to_signed(t_value);
            if (!t_v || !isInRange<int8_t>(*t_v))
                return Error{SchemaCode::Generic, "Value out of range"};
            auto status = encodeI8(static_cast<int8_t>(*t_v), tp_ptr, t_buffer_size);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::Int: {
            auto t_v = to_signed(t_value);
            if (!t_v || !isInRange<int16_t>(*t_v))
                return Error{SchemaCode::Generic, "Value out of range"};
            auto status = encodeI16(static_cast<int16_t>(*t_v), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::UInt:
        case DataType::Word:
        case DataType::Date:
        case DataType::WChar:
        case DataType::Counter:
        case DataType::Timer: {
            auto t_v = to_unsigned(t_value);
            if (!t_v || !isInRange<uint16_t>(*t_v))
                return Error{SchemaCode::Generic, "Value out of range"};
            auto status = encodeU16(static_cast<uint16_t>(*t_v), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::DInt: {
            auto t_v = to_signed(t_value);
            if (!t_v || !isInRange<int32_t>(*t_v))
                return Error{SchemaCode::Generic, "Value out of range"};
            auto status = encodeI32(static_cast<int32_t>(*t_v), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::Time: {
            int64_t ms = 0;
            if (t_value.IsString()) {
                if (!s7codec::parseTimeString(t_value.GetString(), ms))
                    return Error{SchemaCode::Generic, "Invalid TIME"};
            } else {
                auto t_v = to_signed(t_value);
                if (!t_v)
                    return Error{SchemaCode::Generic, "Expected integer for TIME"};
                ms = *t_v;
            }
            if (!isInRange<int32_t>(ms))
                return Error{SchemaCode::Generic, "TIME value out of range"};
            auto status = encodeI32(static_cast<int32_t>(ms), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::UDInt:
        case DataType::DWord:
        case DataType::TimeOfDay: {
            auto t_v = to_unsigned(t_value);
            if (!t_v || !isInRange<uint32_t>(*t_v))
                return Error{SchemaCode::Generic, "Value out of range"};
            auto status = encodeU32(static_cast<uint32_t>(*t_v), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::LInt:
        case DataType::LTime: {
            auto t_v = to_signed(t_value);
            if (!t_v)
                return Error{SchemaCode::Generic, "Expected integer for LINT"};
            auto status = encodeI64(*t_v, tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::ULInt:
        case DataType::LWord:
        case DataType::LTimeOfDay: {
            auto t_v = to_unsigned(t_value);
            if (!t_v)
                return Error{SchemaCode::Generic, "Expected unsigned integer for 64-bit field"};
            auto status = encodeU64(*t_v, tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::Real: {
            auto t_v = to_float(t_value);
            if (!t_v)
                return Error{SchemaCode::Generic, "Expected number for REAL"};
            auto status = encodeReal(static_cast<float>(*t_v), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::LReal: {
            auto t_v = to_float(t_value);
            if (!t_v)
                return Error{SchemaCode::Generic, "Expected number for LREAL"};
            auto status = encodeLReal(*t_v, tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::DateTime: {
            if (!t_value.IsString())
                return Error{SchemaCode::Generic, "Expected timestamp string"};
            std::string s = t_value.GetString();
            std::tm tm{};
            if (s == "now" || s == "NOW")
                tm = sgrn::utils::time::getLocalTime();
            else if (auto parsed = sgrn::utils::time::parseDateTimeInput(s))
                tm = *parsed;
            else
                return Error{SchemaCode::Generic, "Invalid DATETIME"};
            auto status = encodeDateTime(
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday + 1, tp_ptr, t_buffer_size);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::DTL:
            return encodeDtlValue(t_value, tp_ptr, t_buffer_size, t_e);
        case DataType::String:
        case DataType::XString: {
            if (!t_value.IsString())
                return Error{SchemaCode::Generic, "Expected string"};
            std::string s = t_value.GetString();
            std::expected<void, s7codec::CodecStatus> status;
            if (t_field.type == DataType::String)
                status = encodeString(s.c_str(), static_cast<int>(s.length()), t_field.count, tp_ptr, t_buffer_size);
            else
                status = encodeXString(s.c_str(), static_cast<int>(s.length()), t_field.count, tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::WString:
        case DataType::XWString: {
            if (!t_value.IsString())
                return Error{SchemaCode::Generic,
                    fmt::format("Expected string for {}", (t_field.type == DataType::WString ? "WSTRING" : "XWSTRING"))};
            auto wide = sgrn::utils::strings::utf8ToUtf16(t_value.GetString());
            if (!wide)
                return Error{SchemaCode::Generic, "Invalid UTF-8"};
            std::expected<void, s7codec::CodecStatus> status;
            if (t_field.type == DataType::WString)
                status = encodeWString(reinterpret_cast<const uint16_t*>(wide->c_str()), static_cast<int>(wide->size()), t_field.count,
                    tp_ptr, t_buffer_size, t_e);
            else
                status = encodeXWString(reinterpret_cast<const uint16_t*>(wide->c_str()), static_cast<int>(wide->size()), t_field.count,
                    tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::Struct:
            return Error{SchemaCode::Generic, "STRUCT not directly encodable"};
    }
    return {};
}

sgrn::Result<std::string, ::sgrn::scl::Error> decodeFieldAt(
    const DbField& t_field, const uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    serializeFieldToWriter(t_writer, t_field, tp_ptr, t_buffer_size, t_depth);
    return std::string(sb.GetString());
}

sgrn::Result<void, ::sgrn::scl::Error> encodeArrayValue(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    constexpr int kMaxRecursionDepth = 10;
    if (t_depth > kMaxRecursionDepth)
        return Error{SchemaCode::Generic, "Max recursion depth exceeded"};

    if (!t_value.IsArray()) {
        return Error{SchemaCode::Generic, "Expected array for array field"};
    }

    const int max_elements = std::max(0, t_field.count);
    if (!t_field.is_dynamic && t_value.Size() != static_cast<rapidjson::SizeType>(max_elements)) {
        return Error{
            SchemaCode::Generic, fmt::format("Array length mismatch for field '{}' (type {}): expected {} elements, got {} in JSON",
                                     t_field.name, static_cast<int>(t_field.type), max_elements, t_value.Size())};
    }
    if (t_field.is_dynamic && t_value.Size() > static_cast<rapidjson::SizeType>(max_elements)) {
        return Error{SchemaCode::Generic, fmt::format("Dynamic array overflow for field '{}': max capacity is {}, got {} in JSON",
                                              t_field.name, max_elements, t_value.Size())};
    }

    size_t data_offset = 0;
    if (t_field.is_dynamic) {
        if (t_buffer_size < 4)
            return Error{SchemaCode::Generic, "Buffer too small for dynamic header"};
        s7codec::toEndian<uint32_t>(static_cast<uint32_t>(t_value.Size()), tp_ptr, t_e);
        data_offset = 4;
    }

    if (t_field.type == DataType::Bool) {
        // Special case: packed Bool array
        for (rapidjson::SizeType i = 0; i < t_value.Size(); ++i) {
            bool b = false;
            const auto& t_v = t_value[i];
            if (t_v.IsBool())
                b = t_v.GetBool();
            else if (t_v.IsInt())
                b = t_v.GetInt() != 0;
            else if (t_v.IsString())
                b = (std::string(t_v.GetString()) == "true");

            int byte_off = i / 8;
            int bit_off = i % 8;
            if (static_cast<size_t>(byte_off + data_offset) >= t_buffer_size)
                return Error{SchemaCode::Generic, "Buffer overflow in Bool array"};

            auto status = s7codec::encodeBool(b, bit_off, tp_ptr + data_offset + byte_off, t_buffer_size - data_offset - byte_off);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
        }
        return {};
    }

    const int element_size = fieldElementSpanBytes(t_field);

    for (rapidjson::SizeType index = 0; index < t_value.Size(); ++index) {
        size_t offset_at = data_offset + static_cast<size_t>(index * element_size);
        if (offset_at + element_size > t_buffer_size)
            return Error{SchemaCode::Generic, "Buffer overflow"};

        DbField element = t_field;
        element.count = 1; // Treat as scalar for the recursive call
        element.offset = 0;
        element.is_dynamic = false; // The header is already written

        sgrn::Result<void, ::sgrn::scl::Error> status =
            encodeFieldRapidJson(element, t_value[index], tp_ptr + offset_at, element_size, t_depth + 1, t_e);
        if (!status.has_value())
            return status;
    }
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> encodeFieldRapidJson(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    constexpr int kMaxRecursionDepth = 10;
    if (t_depth > kMaxRecursionDepth)
        return Error{SchemaCode::Generic, "Max recursion depth exceeded"};

    if (t_field.type == DataType::Struct) {
        if (t_field.count > 1)
            return encodeArrayValue(t_field, t_value, tp_ptr, t_buffer_size, t_depth, t_e);
        if (!t_value.IsObject())
            return Error{SchemaCode::Generic, "expected object"};
        for (const DbField& child : t_field.children) {
            if (!t_value.HasMember(child.name.c_str()))
                continue;
            if (static_cast<size_t>(child.offset) >= t_buffer_size)
                return Error{SchemaCode::Generic, "offset out of bounds"};
            sgrn::Result<void, ::sgrn::scl::Error> status = encodeFieldRapidJson(
                child, t_value[child.name.c_str()], tp_ptr + child.offset, t_buffer_size - child.offset, t_depth + 1, t_e);
            if (!status.has_value())
                return status;
        }
        return {};
    }
    if (t_field.count > 1) {
        return encodeArrayValue(t_field, t_value, tp_ptr, t_buffer_size, t_depth, t_e);
    }
    return encodeScalarValue(t_field, t_value, tp_ptr, t_buffer_size, t_e);
}

sgrn::Result<void, ::sgrn::scl::Error> encodeFieldAt(
    const DbField& t_field, const std::string& t_value_json, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
    rapidjson::Document doc;
    if (doc.Parse(t_value_json.c_str()).HasParseError())
        return Error{SchemaCode::ParseError, "Invalid JSON"};
    return encodeFieldRapidJson(t_field, doc, tp_ptr, t_buffer_size, t_depth, t_e);
}

std::string decodeDbBuffer(const sgrn::scl::DbSchema& t_reg, const uint8_t* tp_buf, size_t t_buffer_size) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    t_writer.StartObject();
    for (const DbField& t_field : t_reg.fields) {
        t_writer.Key(t_field.name.c_str());
        serializeFieldToWriter(t_writer, t_field, tp_buf + t_field.offset, t_buffer_size - t_field.offset, 0);
    }
    t_writer.EndObject();
    return std::string(sb.GetString());
}

/// Write a single decoded scalar value to a JSON writer.
/// Shared by both the array and scalar serialization paths.
static void writeDecodedValue(rapidjson::Writer<rapidjson::StringBuffer>& t_writer, const s7codec::DecodedValue& t_dv, DataType t_type) {
    if (!t_dv.valid()) {
        t_writer.Null();
        return;
    }
    switch (t_dv.kind()) {
        case s7codec::ValueKind::Bool:
            t_writer.Bool(t_dv.b());
            break;
        case s7codec::ValueKind::SignedInt:
            if (t_type == DataType::Time)
                t_writer.String(s7codec::formatTimeString(static_cast<int32_t>(t_dv.i())).c_str());
            else
                t_writer.Int64(t_dv.i());
            break;
        case s7codec::ValueKind::UnsignedInt:
            if (t_type == DataType::Byte)
                t_writer.String(fmt::format("0x{:02X}", static_cast<uint8_t>(t_dv.u())).c_str());
            else if (t_type == DataType::Word)
                t_writer.String(fmt::format("0x{:04X}", static_cast<uint16_t>(t_dv.u())).c_str());
            else if (t_type == DataType::DWord)
                t_writer.String(fmt::format("0x{:08X}", static_cast<uint32_t>(t_dv.u())).c_str());
            else
                t_writer.Uint64(t_dv.u());
            break;
        case s7codec::ValueKind::Float:
            t_writer.Double(static_cast<double>(t_dv.f()));
            break;
        case s7codec::ValueKind::Double:
            t_writer.Double(t_dv.d());
            break;
        case s7codec::ValueKind::String:
            t_writer.String(t_dv.s().c_str());
            break;
        default:
            t_writer.Null();
    }
}

static void serializeStructToWriter(
    rapidjson::Writer<rapidjson::StringBuffer>& w, const DbField& f, const uint8_t* ptr, size_t buf_size, int depth) {
    if (f.count > 1) {
        w.StartArray();
        int struct_size = fieldElementSpanBytes(f);
        for (int i = 0; i < f.count; ++i) {
            w.StartObject();
            for (const auto& child : f.children) {
                w.Key(child.name.c_str());
                serializeFieldToWriter(
                    w, child, ptr + (i * struct_size) + child.offset, buf_size - (i * struct_size) - child.offset, depth + 1);
            }
            w.EndObject();
        }
        w.EndArray();
    } else {
        w.StartObject();
        for (const auto& child : f.children) {
            w.Key(child.name.c_str());
            serializeFieldToWriter(w, child, ptr + child.offset, buf_size - child.offset, depth + 1);
        }
        w.EndObject();
    }
}

static void serializeStringToWriter(rapidjson::Writer<rapidjson::StringBuffer>& w, const DbField& f, const uint8_t* ptr, size_t buf_size) {
    if (f.count > 1) {
        w.StartArray();
        int elem_size = fieldElementSpanBytes(f);
        for (int i = 0; i < f.count; ++i) {
            auto dv = s7codec::decodeScalar(f.type, ptr + (i * elem_size), buf_size - (i * elem_size), 0, f.string_capacity);
            writeDecodedValue(w, dv, f.type);
        }
        w.EndArray();
    } else {
        auto dv = s7codec::decodeScalar(f.type, ptr, buf_size, 0, f.string_capacity);
        writeDecodedValue(w, dv, f.type);
    }
}

static void serializeScalarOrArrayToWriter(
    rapidjson::Writer<rapidjson::StringBuffer>& w, const DbField& f, const uint8_t* ptr, size_t buf_size) {
    if (f.count > 1) {
        w.StartArray();
        int elem_size = fieldElementSpanBytes(f);
        for (int i = 0; i < f.count; ++i) {
            const uint8_t* p_elem_ptr = ptr + (i * elem_size);
            int b_idx = f.bit_index;
            if (f.type == DataType::Bool) {
                p_elem_ptr = ptr + (i / 8);
                b_idx = i % 8;
            }
            auto dv = s7codec::decodeScalar(f.type, p_elem_ptr, buf_size - static_cast<size_t>(p_elem_ptr - ptr), b_idx);
            writeDecodedValue(w, dv, f.type);
        }
        w.EndArray();
    } else {
        auto dv = s7codec::decodeScalar(f.type, ptr, buf_size, f.bit_index);
        writeDecodedValue(w, dv, f.type);
    }
}

void serializeFieldToWriter(rapidjson::Writer<rapidjson::StringBuffer>& t_writer, const DbField& t_field, const uint8_t* tp_ptr,
    size_t t_buffer_size, int t_depth) {
    if (t_depth > 16) {
        t_writer.String("!!! ERROR: MAX RECURSION DEPTH EXCEEDED !!!");
        return;
    }
    switch (kind_of(t_field)) {
        case FieldKind::Struct:
            return serializeStructToWriter(t_writer, t_field, tp_ptr, t_buffer_size, t_depth);
        case FieldKind::String:
            return serializeStringToWriter(t_writer, t_field, tp_ptr, t_buffer_size);
        case FieldKind::Scalar:
        case FieldKind::Enum:
            return serializeScalarOrArrayToWriter(t_writer, t_field, tp_ptr, t_buffer_size);
    }
}
sgrn::Result<std::vector<uint8_t>, Error> parseHexBytes(const std::string& t_joined) {
    auto res = sgrn::utils::strings::hexToBytes(t_joined);
    if (!res)
        return sgrn::Result<std::vector<uint8_t>, Error>::Error(Error{SchemaCode::Generic, "Invalid hex byte"});
    return *res;
}

std::optional<PlcAddress> parsePlcAddress(const std::string& t_tok) {
    const std::string tok_trimmed = sgrn::utils::strings::trim(t_tok);
    if (tok_trimmed.empty())
        return std::nullopt;
    const std::string up = sgrn::utils::strings::toUpper(t_tok);

    if (up.size() >= 2 && up[0] == 'T' && std::isdigit(static_cast<unsigned char>(up[1]))) {
        std::optional<int> t_n = sgrn::utils::strings::parseInt(up.substr(1));
        if (!t_n.has_value())
            return std::nullopt;
        PlcAddress a;
        a.area = S7AreaTM;
        a.byte_offset = t_n.value();
        a.word_len = S7WLTimer;
        a.byte_count = 2;
        a.label = fmt::format("T{}", t_n.value());
        return a;
    }
    if (up.size() >= 2 && (up[0] == 'C' || up[0] == 'Z') && std::isdigit(static_cast<unsigned char>(up[1]))) {
        std::optional<int> t_n = sgrn::utils::strings::parseInt(up.substr(1));
        if (!t_n.has_value())
            return std::nullopt;
        PlcAddress a;
        a.area = S7AreaCT;
        a.byte_offset = t_n.value();
        a.word_len = S7WLCounter;
        a.byte_count = 2;
        a.label = fmt::format("C{}", t_n.value());
        return a;
    }
    int area_code = -1;
    std::string area_lbl;
    if (up[0] == 'I' || up[0] == 'E') {
        area_code = S7AreaPE;
        area_lbl = "I";
    } else if (up[0] == 'Q' || up[0] == 'A') {
        area_code = S7AreaPA;
        area_lbl = "Q";
    } else if (up[0] == 'M') {
        area_code = S7AreaMK;
        area_lbl = "M";
    }
    if (area_code < 0)
        return std::nullopt;

    const std::string rest = up.substr(1);
    if (rest.empty())
        return std::nullopt;

    PlcAddress a;
    a.area = area_code;

    {
        const auto dot = rest.find('.');
        if (dot != std::string::npos) {
            std::optional<int> byte_v = sgrn::utils::strings::parseInt(rest.substr(0, dot));
            std::optional<int> bit_v = sgrn::utils::strings::parseInt(rest.substr(dot + 1));
            if (byte_v.has_value() && bit_v.has_value() && bit_v.value() >= 0 && bit_v.value() <= 7) {
                a.byte_offset = byte_v.value();
                a.bit_index = bit_v.value();
                a.word_len = S7WLBit;
                a.byte_count = 1;
                a.label = fmt::format("{}{}.{}", area_lbl, byte_v.value(), bit_v.value());
                return a;
            }
        }
    }
    if (rest.size() >= 2) {
        const char sz = rest[0];
        std::optional<int> t_n = sgrn::utils::strings::parseInt(rest.substr(1));
        if (t_n.has_value()) {
            if (sz == 'B') {
                a.byte_offset = t_n.value();
                a.word_len = S7WLByte;
                a.byte_count = 1;
                a.label = fmt::format("{}B{}", area_lbl, t_n.value());
                return a;
            }
            if (sz == 'W') {
                a.byte_offset = t_n.value();
                a.word_len = S7WLWord;
                a.byte_count = 2;
                a.label = fmt::format("{}W{}", area_lbl, t_n.value());
                return a;
            }
            if (sz == 'D') {
                a.byte_offset = t_n.value();
                a.word_len = S7WLDWord;
                a.byte_count = 4;
                a.label = fmt::format("{}D{}", area_lbl, t_n.value());
                return a;
            }
        }
    }
    {
        std::optional<int> t_n = sgrn::utils::strings::parseInt(rest);
        if (t_n.has_value()) {
            a.byte_offset = t_n.value();
            a.word_len = S7WLByte;
            a.byte_count = 1;
            a.label = fmt::format("{}B{}", area_lbl, t_n.value());
            return a;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> parseCLiteral(const std::string& t_raw) {
    std::string s = sgrn::utils::strings::trim(t_raw);
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    if (s.empty())
        return std::nullopt;
    if (s == "true" || s == "TRUE")
        return uint64_t{1};
    if (s == "false" || s == "FALSE")
        return uint64_t{0};
    if (s.size() > 2 && s[0] == '0') {
        if (s[1] == 'b' || s[1] == 'B') {
            uint64_t t_val = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                if (s[i] != '0' && s[i] != '1')
                    return std::nullopt;
                t_val = (t_val << 1) | static_cast<uint64_t>(s[i] - '0');
            }
            return t_val;
        }
        if (s[1] == 'o' || s[1] == 'O') {
            uint64_t t_val = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                if (s[i] < '0' || s[i] > '7')
                    return std::nullopt;
                t_val = (t_val << 3) | static_cast<uint64_t>(s[i] - '0');
            }
            return t_val;
        }
        if (s[1] == 'x' || s[1] == 'X')
            return sgrn::utils::strings::parseUInt64(s, 16);
    }
    return sgrn::utils::strings::parseUInt64(s, 10);
}

bool hasCLiteralPrefix(const std::string& t_val) {
    const std::string s = sgrn::utils::strings::trim(t_val);
    return s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'B' || s[1] == 'o' || s[1] == 'O');
}

std::vector<uint8_t> uint64ToBytesBE(uint64_t t_val, int t_n) {
    std::vector<uint8_t> out(t_n, 0);
    for (int i = t_n - 1; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(t_val & 0xFF);
        t_val >>= 8;
    }
    return out;
}

std::string byteToBinStr(uint8_t t_b) {
    std::string s;
    s.reserve(9);
    for (int i = 7; i >= 0; --i) {
        s += (((t_b >> i) & 1) ? '1' : '0');
        if (i == 4)
            s += ' ';
    }
    return s;
}

std::optional<PlcDbRawAddr> parsePlcDbRawAddr(const std::string& t_target) {
    const auto dot = t_target.find('.');
    if (dot == std::string::npos)
        return std::nullopt;
    const std::string db_part = t_target.substr(0, dot);
    const std::string off_part = t_target.substr(dot + 1);
    if (off_part.empty())
        return std::nullopt;
    for (const char c : off_part)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
    const std::string up = sgrn::utils::strings::toUpper(db_part);
    if (up.rfind("DB", 0) != 0)
        return std::nullopt;
    const auto db_n = sgrn::utils::strings::parseInt(up.substr(2));
    if (!db_n.has_value())
        return std::nullopt;
    const auto off_n = sgrn::utils::strings::parseInt(off_part);
    if (!off_n.has_value())
        return std::nullopt;
    return PlcDbRawAddr{static_cast<uint16_t>(*db_n), off_n.value()};
}

std::optional<uint16_t> parseDbRef(const std::string& t_val) {
    std::string val_trimmed = sgrn::utils::strings::trim(t_val);
    if (t_val.rfind("DB", 0) == 0 || t_val.rfind("db", 0) == 0)
        val_trimmed = val_trimmed.substr(2);
    auto res = sgrn::utils::strings::parseInt(t_val);
    if (res && *res >= 0 && *res <= 65535)
        return static_cast<uint16_t>(*res);
    return std::nullopt;
}

std::optional<uint16_t> resolveDbRef(const std::string& t_token, const std::map<std::string, uint16_t>& t_name_to_number) {
    if (auto db = parseDbRef(t_token))
        return db;
    auto it = t_name_to_number.find(t_token);
    if (it != t_name_to_number.end())
        return it->second;
    return std::nullopt;
}

std::optional<std::pair<uint16_t, std::string>> parseFieldTarget(
    const std::string& t_target, const std::map<std::string, uint16_t>& t_name_to_number) {
    const auto dot = t_target.find('.');
    if (dot == std::string::npos)
        return std::nullopt;
    std::string db_part = t_target.substr(0, dot);
    std::string field_part = t_target.substr(dot + 1);
    if (field_part.empty())
        return std::nullopt;
    if (auto db = resolveDbRef(db_part, t_name_to_number))
        return std::make_pair(*db, field_part);
    return std::nullopt;
}

std::string_view plcAreaName(int t_area) {
    if (t_area == S7AreaPE)
        return "Inputs  (I/E)";
    if (t_area == S7AreaPA)
        return "Outputs (Q/A)";
    if (t_area == S7AreaMK)
        return "Merkers (M)  ";
    if (t_area == S7AreaTM)
        return "Timers  (T)  ";
    if (t_area == S7AreaCT)
        return "Counters(C)  ";
    if (t_area == S7AreaDB)
        return "DataBlock(DB)";
    return "Unknown      ";
}

} // namespace sgrn::scl
