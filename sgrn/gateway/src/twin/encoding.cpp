#include <fmt/format.h>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/time_utils.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>

namespace sgrn::gateway::twin
{
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbField;
using ::sgrn::scl::Error;
using ::sgrn::scl::ErrorCode;
using ::sgrn::scl::SchemaCode;

constexpr int kMaxRecursionDepth = 16;
constexpr const char str_true_upper[] = "TRUE";
constexpr const char str_true_lower[] = "true";
constexpr const char str_false_upper[] = "FALSE";
constexpr const char str_false_lower[] = "false";
constexpr const char prefix_bin[] = "bin:";
constexpr const char prefix_hex[] = "hex:";
constexpr const char prefix_s7_time[] = "#T";
constexpr const char prefix_s7_time_lower[] = "#t";

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
        if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &year, &month, &day, &hour, &minute, &second, &ns) >= 3) {
            dtl.year = static_cast<uint16_t>(year);
            dtl.month = static_cast<uint8_t>(month);
            dtl.day = static_cast<uint8_t>(day);
            dtl.hour = static_cast<uint8_t>(hour);
            dtl.minute = static_cast<uint8_t>(minute);
            dtl.second = static_cast<uint8_t>(second);
            dtl.nanosecond = static_cast<uint32_t>(ns);

            // ✅ Compute weekday (Siemens: 1=Sunday … 7=Saturday)
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
                status =
                    encodeString(s.c_str(), static_cast<int>(s.length()), (t_field.count > 0 ? t_field.count : 254), tp_ptr, t_buffer_size);
            else
                status = encodeXString(
                    s.c_str(), static_cast<int>(s.length()), (t_field.count > 0 ? t_field.count : 254), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::WString:
        case DataType::XWString: {
            if (!t_value.IsString())
                return Error{SchemaCode::Generic, "Expected string"};
            auto wide = sgrn::utils::strings::utf8ToUtf16(t_value.GetString());
            if (!wide)
                return Error{SchemaCode::Generic, "Invalid UTF-8"};
            std::expected<void, s7codec::CodecStatus> status;
            if (t_field.type == DataType::WString)
                status = encodeWString(reinterpret_cast<const uint16_t*>(wide->c_str()), static_cast<int>(wide->size()),
                    (t_field.count > 0 ? t_field.count : 16382), tp_ptr, t_buffer_size, t_e);
            else
                status = encodeXWString(reinterpret_cast<const uint16_t*>(wide->c_str()), static_cast<int>(wide->size()),
                    (t_field.count > 0 ? t_field.count : 16382), tp_ptr, t_buffer_size, t_e);
            if (!status.has_value())
                return Error{SchemaCode::Generic, toString(status.error())};
            return {};
        }
        case DataType::Struct:
            return Error{SchemaCode::Generic, "STRUCT not directly encodable"};
    }
    return {};
}

sgrn::Result<void, ::sgrn::scl::Error> encodeArrayValue(
    const DbField& t_field, const rapidjson::Value& t_value, uint8_t* tp_ptr, size_t t_buffer_size, int t_depth, s7codec::Endian t_e) {
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

    const int element_size = std::max(1, t_field.type == DataType::Struct ? t_field.struct_size : s7codec::primitiveSize(t_field.type));

    for (rapidjson::SizeType index = 0; index < t_value.Size(); ++index) {
        size_t offset_at = data_offset + static_cast<size_t>(index * element_size);
        int b_idx = t_field.bit_index;

        if (offset_at + element_size > t_buffer_size)
            return Error{SchemaCode::Generic, "Buffer overflow"};

        DbField element = t_field;
        element.count = 1; // Treat as scalar for the recursive call
        element.offset = 0;
        element.bit_index = b_idx;
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
    if (t_field.count > 1 && t_field.type != DataType::String && t_field.type != DataType::WString && t_field.type != DataType::XString &&
        t_field.type != DataType::XWString) {
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
            rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
            val.Accept(writer);
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
    if (val == str_true_lower || val == str_true_upper)
        return "true";
    if (val == str_false_lower || val == str_false_upper)
        return "false";

    if (val.size() > std::string_view(prefix_bin).size() && val.substr(0, std::string_view(prefix_bin).size()) == prefix_bin) {
        uint64_t t_v = 0;
        for (size_t i = std::string_view(prefix_bin).size(); i < val.size(); ++i) {
            if (val[i] == '0' || val[i] == '1')
                t_v = (t_v << 1) | (val[i] - '0');
        }
        return std::to_string(t_v);
    }
    if (val.size() > std::string_view(prefix_hex).size() && val.substr(0, std::string_view(prefix_hex).size()) == prefix_hex) {
        if (auto num = sgrn::utils::strings::parseUInt64(val.substr(std::string_view(prefix_hex).size()), 16))
            return std::to_string(*num);
    }
    if (val.size() > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
        if (auto num = sgrn::utils::strings::parseUInt64(val, 16))
            return std::to_string(*num);
    }
    if (val.size() > std::string_view(prefix_s7_time).size() &&
        (val.substr(0, std::string_view(prefix_s7_time).size()) == prefix_s7_time ||
            val.substr(0, std::string_view(prefix_s7_time_lower).size()) == prefix_s7_time_lower)) {
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
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        writer.StartArray();
        std::istringstream in(trimmed);
        std::string part;
        while (std::getline(in, part, ',')) {
            std::string sem = parseSemanticValue(part);
            // MED-3: Avoid a full Document::Parse + heap allocation per token.
            // Try to write primitives directly; only fall back to Document for
            // values that are already valid JSON objects/arrays.
            if (!sem.empty() && sem.front() == '"') {
                // String token — strip surrounding quotes
                std::string inner = sem.size() >= 2 ? sem.substr(1, sem.size() - 2) : "";
                writer.String(inner.c_str(), static_cast<rapidjson::SizeType>(inner.size()));
            } else if (sem == "true") {
                writer.Bool(true);
            } else if (sem == "false") {
                writer.Bool(false);
            } else if (sem == "null") {
                writer.Null();
            } else {
                // Numeric or complex — parse once and accept
                rapidjson::Document doc;
                if (!doc.Parse(sem.c_str()).HasParseError())
                    doc.Accept(writer);
                else
                    writer.Null();
            }
        }
        writer.EndArray();
        return sb.GetString();
    }
    return parseSemanticValue(trimmed);
}

sgrn::Result<void, ::sgrn::scl::Error> encodeValue(
    const DbField& t_field, const std::string& t_value, uint8_t* tp_buffer_ptr, size_t t_buffer_size) {
    return encodeFieldAt(t_field, t_value, tp_buffer_ptr, t_buffer_size);
}

} // namespace sgrn::gateway::twin
