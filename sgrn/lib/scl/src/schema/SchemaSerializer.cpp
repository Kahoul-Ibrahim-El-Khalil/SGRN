#include <fmt/core.h>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/utils.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sgrn::scl
{

namespace detail
{

static int fieldSpanBytes(const DbField& t_field) {
    if (t_field.type == DataType::Struct)
        return std::max(1, static_cast<int>(t_field.struct_size)) * std::max(1, static_cast<int>(t_field.count));
    // For strings, struct_size holds the per-element byte span (after offset-tracker fix).
    if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
        t_field.type == DataType::XWString) {
        const int elem_span = t_field.struct_size > 0 ? static_cast<int>(t_field.struct_size) : [&]() -> int {
            auto opt = s7codec::typeSpanBytes(t_field.type, t_field.string_capacity > 0 ? t_field.string_capacity : t_field.count);
            return opt ? static_cast<int>(*opt) : 0;
        }();
        return elem_span * std::max(1, static_cast<int>(t_field.count));
    }
    auto opt = s7codec::typeSpanBytes(t_field.type, t_field.count);
    return opt ? static_cast<int>(*opt) : 0;
}

static void resolveUdtInField(DbField& t_field, const PlcSchemaStore& t_registry) {
    if (t_field.type == DataType::Struct && !t_field.udt_name.empty() && t_field.children.empty()) {
        auto udt_res = t_registry.getUdtByName(t_field.udt_name);
        if (udt_res.has_value()) {
            const UdtDefinition* p_udt = udt_res.value();
            t_field.children = p_udt->fields;
            t_field.struct_size = p_udt->size_bytes;
        }
    }
    for (auto& child : t_field.children) {
        resolveUdtInField(child, t_registry);
    }
}

// --- RapidJSON Serialization Templates ---

template <typename Writer>
static void serializeDbField(Writer& t_writer, const DbField& t_field) {
    t_writer.StartObject();
    t_writer.Key("name");
    t_writer.String(t_field.name.c_str());
    t_writer.Key("offset");
    t_writer.Int(t_field.offset);
    t_writer.Key("bit");
    t_writer.Int(t_field.bit_index);
    t_writer.Key("type");
    t_writer.String(s7codec::s7TypeToString(t_field.type));

    if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
        t_field.type == DataType::XWString) {
        // After the offset-tracker fix:
        //   scalar string:  count=1,  string_capacity=chars, struct_size=byte_span
        //   string array:   count=N,  string_capacity=chars, struct_size=per-elem-byte_span
        // Use count>1 as the array discriminator; always emit string_capacity as 'capacity'.
        const bool t_is_str_array = (t_field.count > 1);
        t_writer.Key("count");
        t_writer.Int(t_field.count);
        t_writer.Key("capacity");
        // Prefer string_capacity; fall back to deriving from struct_size if absent.
        if (t_field.string_capacity > 0) {
            t_writer.Int(t_field.string_capacity);
        } else if (t_field.struct_size > 0) {
            // Reverse-engineer char capacity from byte span.
            int hdr = (t_field.type == DataType::String) ? 2 : (t_field.type == DataType::WString) ? 4 : 8; // XString / XWString
            int wscale = (t_field.type == DataType::WString || t_field.type == DataType::XWString) ? 2 : 1;
            t_writer.Int((t_field.struct_size - hdr) / wscale);
        } else {
            t_writer.Int(t_field.count); // legacy fallback
        }
        (void)t_is_str_array; // used for documentation only
    } else {
        t_writer.Key("count");
        t_writer.Int(t_field.count);
    }
    if (!t_field.udt_name.empty()) {
        t_writer.Key("udt_name");
        t_writer.String(t_field.udt_name.c_str());
    }
    if (!t_field.children.empty()) {
        t_writer.Key("children");
        t_writer.StartArray();
        for (const auto& child : t_field.children)
            serializeDbField(t_writer, child);
        t_writer.EndArray();
    }
    if (t_field.struct_size > 0) {
        t_writer.Key("struct_size");
        t_writer.Int(t_field.struct_size);
    }
    if (t_field.unit.has_value()) {
        t_writer.Key("unit");
        t_writer.String(t_field.unit.value().c_str());
    }
    if (t_field.dimension.has_value()) {
        t_writer.Key("dimension");
        t_writer.String(t_field.dimension.value().c_str());
    }
    if (t_field.description.has_value()) {
        t_writer.Key("description");
        t_writer.String(t_field.description.value().c_str());
    }
    if (t_field.precision.has_value()) {
        t_writer.Key("precision");
        t_writer.Int(t_field.precision.value());
    }
    if (t_field.nominal.has_value()) {
        t_writer.Key("nominal");
        t_writer.Double(t_field.nominal.value());
    }
    if (t_field.alarm_lo.has_value() && t_field.alarm_hi.has_value()) {
        t_writer.Key("alarm_lo");
        t_writer.Double(t_field.alarm_lo.value());
        t_writer.Key("alarm_hi");
        t_writer.Double(t_field.alarm_hi.value());
    }
    if (t_field.is_label) {
        t_writer.Key("is_label");
        t_writer.Bool(true);
    }
    if (t_field.is_transient) {
        t_writer.Key("is_transient");
        t_writer.Bool(true);
    }
    if (t_field.is_read_only) {
        t_writer.Key("is_read_only");
        t_writer.Bool(true);
    }
    if (t_field.min_val.has_value()) {
        t_writer.Key("min");
        t_writer.Double(t_field.min_val.value());
    }
    if (t_field.max_val.has_value()) {
        t_writer.Key("max");
        t_writer.Double(t_field.max_val.value());
    }
    if (!t_field.enum_map.empty()) {
        t_writer.Key("enum");
        t_writer.StartObject();
        for (const auto& [k, v] : t_field.enum_map) {
            t_writer.Key(std::to_string(k).c_str());
            t_writer.String(v.c_str());
        }
        t_writer.EndObject();
    }
    if (t_field.endianness != s7codec::Endian::Big) {
        t_writer.Key("endianness");
        t_writer.String(t_field.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_field.trigger_events) {
        t_writer.Key("trigger_events");
        t_writer.Bool(true);
    }
    if (t_field.is_dynamic) {
        t_writer.Key("is_dynamic");
        t_writer.Bool(true);
    }
    t_writer.EndObject();
}

template <typename Writer>
static void serializeUdtDefinition(Writer& t_writer, const UdtDefinition& t_udt) {
    t_writer.StartObject();
    t_writer.Key("udt_number");
    t_writer.Int(t_udt.udt_number);
    t_writer.Key("name");
    t_writer.String(t_udt.name.c_str());
    t_writer.Key("size_bytes");
    t_writer.Int(t_udt.size_bytes);
    t_writer.Key("fields");
    t_writer.StartArray();
    for (const auto& t_field : t_udt.fields)
        serializeDbField(t_writer, t_field);
    t_writer.EndArray();
    if (t_udt.endianness != s7codec::Endian::Big) {
        t_writer.Key("endianness");
        t_writer.String(t_udt.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_udt.trigger_events) {
        t_writer.Key("trigger_events");
        t_writer.Bool(true);
    }
    if (t_udt.is_scalar_alias) {
        t_writer.Key("is_scalar_alias");
        t_writer.Bool(true);
        t_writer.Key("scalar_type");
        t_writer.String(s7codec::s7TypeToString(t_udt.scalar_type));
    }
    if (!t_udt.enum_map.empty()) {
        t_writer.Key("enum");
        t_writer.StartObject();
        for (const auto& [key, value] : t_udt.enum_map) {
            t_writer.Key(std::to_string(key).c_str());
            t_writer.String(value.c_str());
        }
        t_writer.EndObject();
    }
    if (t_udt.unit.has_value()) {
        t_writer.Key("unit");
        t_writer.String(t_udt.unit.value().c_str());
    }
    if (t_udt.dimension.has_value()) {
        t_writer.Key("dimension");
        t_writer.String(t_udt.dimension.value().c_str());
    }
    if (t_udt.description.has_value()) {
        t_writer.Key("description");
        t_writer.String(t_udt.description.value().c_str());
    }
    if (t_udt.precision.has_value()) {
        t_writer.Key("precision");
        t_writer.Int(t_udt.precision.value());
    }
    if (t_udt.nominal.has_value()) {
        t_writer.Key("nominal");
        t_writer.Double(t_udt.nominal.value());
    }
    if (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value()) {
        t_writer.Key("alarm_lo");
        t_writer.Double(t_udt.alarm_lo.value());
        t_writer.Key("alarm_hi");
        t_writer.Double(t_udt.alarm_hi.value());
    }
    if (t_udt.min_val.has_value()) {
        t_writer.Key("min");
        t_writer.Double(t_udt.min_val.value());
    }
    if (t_udt.max_val.has_value()) {
        t_writer.Key("max");
        t_writer.Double(t_udt.max_val.value());
    }
    t_writer.EndObject();
}

template <typename Writer>
static void serializeDbSchema(Writer& t_writer, const DbSchema& t_db, bool t_headers_only) {
    t_writer.StartObject();
    t_writer.Key("number");
    t_writer.Int(t_db.db_number);
    t_writer.Key("name");
    t_writer.String(t_db.db_name.c_str());
    t_writer.Key("size_bytes");
    t_writer.Int(t_db.size_bytes);
    if (!t_db.source_file.empty()) {
        t_writer.Key("source_file");
        t_writer.String(t_db.source_file.c_str());
    }
    if (t_db.endianness != s7codec::Endian::Big) {
        t_writer.Key("endianness");
        t_writer.String(t_db.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_db.trigger_events) {
        t_writer.Key("trigger_events");
        t_writer.Bool(true);
    }
    if (t_db.modbus_area != ModbusArea::None) {
        t_writer.Key("modbus_area");
        switch (t_db.modbus_area) {
            case sgrn::scl::ModbusArea::Holding:
                t_writer.String("holding");
                break;
            case sgrn::scl::ModbusArea::Input:
                t_writer.String("input");
                break;
            case sgrn::scl::ModbusArea::Coil:
                t_writer.String("coil");
                break;
            case sgrn::scl::ModbusArea::Discrete:
                t_writer.String("discrete");
                break;
            default:
                break;
        }
    }
    if (!t_headers_only) {
        t_writer.Key("fields");
        t_writer.StartArray();
        for (const auto& t_field : t_db.fields)
            serializeDbField(t_writer, t_field);
        t_writer.EndArray();
    }
    t_writer.EndObject();
}

template <typename Writer>
static void serializePlcTag(Writer& t_writer, const PlcTag& t_tag) {
    t_writer.StartObject();
    t_writer.Key("name");
    t_writer.String(t_tag.name.c_str());
    t_writer.Key("table");
    t_writer.String(t_tag.table_name.c_str());
    t_writer.Key("type");
    t_writer.String(t_tag.type_str.c_str());
    if (!t_tag.remark.empty()) {
        t_writer.Key("remark");
        t_writer.String(t_tag.remark.c_str());
    }
    t_writer.Key("address");
    t_writer.String(t_tag.addr.label.c_str());
    t_writer.EndObject();
}

template <typename Writer>
static void doSerialize(Writer& t_writer, const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number, bool t_headers_only) {
    t_writer.StartObject();

    // DBs
    t_writer.Key("dbs");
    t_writer.StartArray();
    int accessible_dbs = 0;

    for (const auto& [num, t_db] : t_registry.dbs()) {
        if (t_db_number.has_value() && num != *t_db_number)
            continue;

        serializeDbSchema(t_writer, t_db, t_headers_only);

        accessible_dbs++;
    }
    t_writer.EndArray();

    // UDTs and Tags (only if not filtering)
    if (!t_db_number.has_value()) {
        t_writer.Key("udts");
        t_writer.StartArray();
        for (const UdtDefinition& p_udt : t_registry.udts())
            serializeUdtDefinition(t_writer, p_udt);
        t_writer.EndArray();

        t_writer.Key("tags");
        t_writer.StartArray();
        for (const auto& [name, t_tag] : t_registry.tags())
            serializePlcTag(t_writer, t_tag);
        t_writer.EndArray();
    }

    // Declared dimension vocabulary (union across compiled files).
    if (!t_db_number.has_value() && !t_registry.dimensions().empty()) {
        t_writer.Key("dimensions");
        t_writer.StartArray();
        for (const auto& dim : t_registry.dimensions())
            t_writer.String(dim.c_str());
        t_writer.EndArray();
    }

    // Summary
    t_writer.Key("summary");
    t_writer.StartObject();
    t_writer.Key("total_dbs");
    t_writer.Int(static_cast<int>(t_registry.dbs().size()));
    t_writer.Key("total_tags");
    t_writer.Int(static_cast<int>(t_registry.tags().size()));
    t_writer.Key("accessible_dbs");
    t_writer.Int(accessible_dbs);
    t_writer.Key("total_udts");
    t_writer.Int(static_cast<int>(t_registry.udts().size()));
    t_writer.Key("warnings");
    t_writer.Int(static_cast<int>(t_registry.warnings().size()));
    if (t_db_number.has_value()) {
        t_writer.Key("filtered_db");
        t_writer.Int(*t_db_number);
    }
    t_writer.EndObject();

    t_writer.EndObject();
}

} // namespace detail

// --- SchemaSerializer Implementation ---

std::string SchemaSerializer::udtToJson(const UdtDefinition& t_udt) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    detail::serializeUdtDefinition(t_writer, t_udt);
    return sb.GetString();
}

std::string SchemaSerializer::dbToJson(const DbSchema& t_db) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    detail::serializeDbSchema(t_writer, t_db, false);
    return sb.GetString();
}

std::string SchemaSerializer::tagToJson(const PlcTag& t_tag) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    detail::serializePlcTag(t_writer, t_tag);
    return sb.GetString();
}

std::string SchemaSerializer::serialize(
    const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number, bool t_headers_only, bool t_pretty) {
    rapidjson::StringBuffer sb;
    if (t_pretty) {
        rapidjson::PrettyWriter<rapidjson::StringBuffer> t_writer(sb);
        detail::doSerialize(t_writer, t_registry, t_db_number, t_headers_only);
    } else {
        rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
        detail::doSerialize(t_writer, t_registry, t_db_number, t_headers_only);
    }
    return sb.GetString();
}

// --- Deserialization (rapidjson) ---

static sgrn::Result<DbField, scl::SclError> fieldFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return scl::SclError::Generic;

    DbField t_field;
    if (!t_node.HasMember("name") || !t_node["name"].IsString())
        return scl::SclError::Generic;
    t_field.name = t_node["name"].GetString();

    if (t_node.HasMember("offset"))
        t_field.offset = t_node["offset"].GetInt();
    if (t_node.HasMember("bit"))
        t_field.bit_index = t_node["bit"].GetInt();
    else if (t_node.HasMember("bit_index"))
        t_field.bit_index = t_node["bit_index"].GetInt();
    if (t_node.HasMember("count"))
        t_field.count = t_node["count"].GetInt();
    if (t_node.HasMember("udt_name") && t_node["udt_name"].IsString())
        t_field.udt_name = t_node["udt_name"].GetString();
    if (t_node.HasMember("struct_size"))
        t_field.struct_size = t_node["struct_size"].GetInt();
    if (t_node.HasMember("unit") && t_node["unit"].IsString())
        t_field.unit = t_node["unit"].GetString();
    if (t_node.HasMember("dimension") && t_node["dimension"].IsString())
        t_field.dimension = t_node["dimension"].GetString();
    if (t_node.HasMember("description") && t_node["description"].IsString())
        t_field.description = t_node["description"].GetString();
    if (t_node.HasMember("precision") && t_node["precision"].IsInt())
        t_field.precision = t_node["precision"].GetInt();
    if (t_node.HasMember("nominal") && t_node["nominal"].IsNumber())
        t_field.nominal = t_node["nominal"].GetDouble();
    if (t_node.HasMember("alarm_lo") && t_node["alarm_lo"].IsNumber())
        t_field.alarm_lo = t_node["alarm_lo"].GetDouble();
    if (t_node.HasMember("alarm_hi") && t_node["alarm_hi"].IsNumber())
        t_field.alarm_hi = t_node["alarm_hi"].GetDouble();
    if (t_node.HasMember("is_label") && t_node["is_label"].IsBool())
        t_field.is_label = t_node["is_label"].GetBool();
    if (t_node.HasMember("is_transient") && t_node["is_transient"].IsBool())
        t_field.is_transient = t_node["is_transient"].GetBool();
    if (t_node.HasMember("is_read_only") && t_node["is_read_only"].IsBool())
        t_field.is_read_only = t_node["is_read_only"].GetBool();
    if (t_node.HasMember("min") && t_node["min"].IsNumber())
        t_field.min_val = t_node["min"].GetDouble();
    if (t_node.HasMember("max") && t_node["max"].IsNumber())
        t_field.max_val = t_node["max"].GetDouble();
    if (t_node.HasMember("enum") && t_node["enum"].IsObject()) {
        for (auto it = t_node["enum"].MemberBegin(); it != t_node["enum"].MemberEnd(); ++it) {
            try {
                int key = std::stoi(it->name.GetString());
                if (it->value.IsString()) {
                    t_field.enum_map[key] = it->value.GetString();
                }
            } catch (...) {
            }
        }
    }
    if (t_node.HasMember("endianness") && t_node["endianness"].IsString()) {
        std::string e = t_node["endianness"].GetString();
        t_field.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.HasMember("trigger_events") && t_node["trigger_events"].IsBool()) {
        t_field.trigger_events = t_node["trigger_events"].GetBool();
    }
    if (t_node.HasMember("is_dynamic") && t_node["is_dynamic"].IsBool()) {
        t_field.is_dynamic = t_node["is_dynamic"].GetBool();
    }

    if (!t_node.HasMember("type") || !t_node["type"].IsString())
        return scl::SclError::Generic;

    std::optional<DataType> type = parseDataType(t_node["type"].GetString());
    if (!type.has_value()) {
        t_field.type = DataType::Struct;
        t_field.udt_name = t_node["type"].GetString();
    } else {
        t_field.type = type.value();
    }

    // For string types: 'capacity' in JSON is the char capacity (as emitted by serializeDbField).
    // Restore string_capacity and recompute struct_size (per-element byte span) so that
    // the deserialized DbField matches what the SCL parser would produce.
    const bool t_is_json_string = (t_field.type == DataType::String || t_field.type == DataType::WString ||
                                   t_field.type == DataType::XString || t_field.type == DataType::XWString);
    if (t_is_json_string && t_node.HasMember("capacity") && t_node["capacity"].IsInt()) {
        const int char_cap = t_node["capacity"].GetInt();
        t_field.string_capacity = char_cap;
        // Ensure count >= 1 (scalar strings serialized with count=1)
        if (t_field.count < 1)
            t_field.count = 1;
        // Recompute the per-element byte span
        auto span_opt = s7codec::typeSpanBytes(t_field.type, char_cap);
        t_field.struct_size = span_opt ? *span_opt : 0;
    }

    if (t_node.HasMember("children")) {
        if (!t_node["children"].IsArray())
            return scl::SclError::Generic;
        for (const auto& child_node : t_node["children"].GetArray()) {
            sgrn::Result<DbField, scl::SclError> child = fieldFromJson(child_node);
            if (child.hasError()) {
                return Error(child.error());
            }
            t_field.children.push_back(std::move(child.value()));
        }
    }

    return t_field;
}

static int extractTrailingNumber(const std::string& t_value) {
    std::smatch m;
    static const std::regex kSuffixRe(R"((\d+)\s*$)");
    if (std::regex_search(t_value, m, kSuffixRe))
        return sgrn::utils::strings::parseInt(m[1].str()).value_or(0);
    return 0;
}

sgrn::Result<UdtDefinition, scl::SclError> SchemaSerializer::udtFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return scl::SclError::Generic;

    UdtDefinition p_udt;
    if (t_node.HasMember("udt_number"))
        p_udt.udt_number = t_node["udt_number"].GetInt();
    if (t_node.HasMember("name") && t_node["name"].IsString())
        p_udt.name = t_node["name"].GetString();
    if (t_node.HasMember("size_bytes"))
        p_udt.size_bytes = t_node["size_bytes"].GetInt();

    if (t_node.HasMember("fields")) {
        if (!t_node["fields"].IsArray())
            return scl::SclError::Generic;
        for (const auto& field_node : t_node["fields"].GetArray()) {
            sgrn::Result<DbField, scl::SclError> t_field = fieldFromJson(field_node);
            if (t_field.hasError()) {
                return Error(t_field.error());
            }
            p_udt.fields.push_back(std::move(t_field.value()));
        }
    }

    if (t_node.HasMember("endianness") && t_node["endianness"].IsString()) {
        std::string e = t_node["endianness"].GetString();
        p_udt.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.HasMember("trigger_events") && t_node["trigger_events"].IsBool()) {
        p_udt.trigger_events = t_node["trigger_events"].GetBool();
    }
    if (t_node.HasMember("is_scalar_alias") && t_node["is_scalar_alias"].IsBool()) {
        p_udt.is_scalar_alias = t_node["is_scalar_alias"].GetBool();
    }
    if (t_node.HasMember("scalar_type") && t_node["scalar_type"].IsString()) {
        if (auto opt = s7codec::toType(t_node["scalar_type"].GetString()))
            p_udt.scalar_type = *opt;
    }
    if (t_node.HasMember("enum") && t_node["enum"].IsObject()) {
        for (auto it = t_node["enum"].MemberBegin(); it != t_node["enum"].MemberEnd(); ++it) {
            try {
                const int key = std::stoi(it->name.GetString());
                if (it->value.IsString())
                    p_udt.enum_map[key] = it->value.GetString();
            } catch (...) {
            }
        }
    }
    if (t_node.HasMember("unit") && t_node["unit"].IsString())
        p_udt.unit = t_node["unit"].GetString();
    if (t_node.HasMember("dimension") && t_node["dimension"].IsString())
        p_udt.dimension = t_node["dimension"].GetString();
    if (t_node.HasMember("description") && t_node["description"].IsString())
        p_udt.description = t_node["description"].GetString();
    if (t_node.HasMember("precision") && t_node["precision"].IsInt())
        p_udt.precision = t_node["precision"].GetInt();
    if (t_node.HasMember("nominal") && t_node["nominal"].IsNumber())
        p_udt.nominal = t_node["nominal"].GetDouble();
    if (t_node.HasMember("alarm_lo") && t_node["alarm_lo"].IsNumber())
        p_udt.alarm_lo = t_node["alarm_lo"].GetDouble();
    if (t_node.HasMember("alarm_hi") && t_node["alarm_hi"].IsNumber())
        p_udt.alarm_hi = t_node["alarm_hi"].GetDouble();
    if (t_node.HasMember("min") && t_node["min"].IsNumber())
        p_udt.min_val = t_node["min"].GetDouble();
    if (t_node.HasMember("max") && t_node["max"].IsNumber())
        p_udt.max_val = t_node["max"].GetDouble();

    return p_udt;
}

sgrn::Result<DbSchema, scl::SclError> SchemaSerializer::dbFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return scl::SclError::Generic;

    DbSchema t_db;
    if (t_node.HasMember("number"))
        t_db.db_number = t_node["number"].GetInt();
    else if (t_node.HasMember("db_number"))
        t_db.db_number = t_node["db_number"].GetInt();
    if (t_node.HasMember("name") && t_node["name"].IsString())
        t_db.db_name = t_node["name"].GetString();
    else if (t_node.HasMember("db_name") && t_node["db_name"].IsString())
        t_db.db_name = t_node["db_name"].GetString();
    if (t_node.HasMember("size_bytes"))
        t_db.size_bytes = t_node["size_bytes"].GetInt();
    if (t_node.HasMember("source_file") && t_node["source_file"].IsString())
        t_db.source_file = t_node["source_file"].GetString();
    if (t_node.HasMember("endianness") && t_node["endianness"].IsString()) {
        std::string e = t_node["endianness"].GetString();
        t_db.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.HasMember("trigger_events") && t_node["trigger_events"].IsBool()) {
        t_db.trigger_events = t_node["trigger_events"].GetBool();
    }
    if (t_node.HasMember("modbus_area") && t_node["modbus_area"].IsString()) {
        std::string a = t_node["modbus_area"].GetString();
        if (a == "holding")
            t_db.modbus_area = sgrn::scl::ModbusArea::Holding;
        else if (a == "input")
            t_db.modbus_area = sgrn::scl::ModbusArea::Input;
        else if (a == "coil")
            t_db.modbus_area = sgrn::scl::ModbusArea::Coil;
        else if (a == "discrete")
            t_db.modbus_area = sgrn::scl::ModbusArea::Discrete;
    }

    if (t_node.HasMember("fields")) {
        if (!t_node["fields"].IsArray())
            return scl::SclError::Generic;
        for (const auto& field_node : t_node["fields"].GetArray()) {
            sgrn::Result<DbField, scl::SclError> t_field = fieldFromJson(field_node);
            if (t_field.hasError()) {
                return Error(t_field.error());
            }
            t_db.fields.push_back(std::move(t_field.value()));
        }
    }

    if (t_db.size_bytes == 0) {
        int max_end = 0;
        for (const DbField& t_field : t_db.fields)
            max_end = std::max(max_end, t_field.offset + detail::fieldSpanBytes(t_field));
        t_db.size_bytes = max_end;
    }
    return t_db;
}

sgrn::Result<PlcTag, scl::SclError> SchemaSerializer::tagFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return scl::SclError::Generic;
    PlcTag t_tag;
    if (!t_node.HasMember("name") || !t_node["name"].IsString())
        return scl::SclError::Generic;
    t_tag.name = t_node["name"].GetString();
    if (t_node.HasMember("table") && t_node["table"].IsString())
        t_tag.table_name = t_node["table"].GetString();
    if (t_node.HasMember("type") && t_node["type"].IsString())
        t_tag.type_str = t_node["type"].GetString();
    if (t_node.HasMember("remark") && t_node["remark"].IsString())
        t_tag.remark = t_node["remark"].GetString();
    if (t_node.HasMember("address") && t_node["address"].IsString()) {
        if (auto addr = parsePlcAddress(t_node["address"].GetString()))
            t_tag.addr = *addr;
        else
            return scl::SclError::Generic;
    }
    return t_tag;
}

sgrn::Result<void, scl::SclError> SchemaSerializer::deserialize(PlcSchemaStore& t_registry, const rapidjson::Value& t_root) {
    if (!t_root.IsObject() && !t_root.IsArray())
        return scl::SclError::Generic;

    auto load_udts = [&](const rapidjson::Value* tp_value) -> sgrn::Result<void, scl::SclError> {
        if (!tp_value)
            return {};
        if (!tp_value->IsArray())
            return scl::SclError::Generic;
        for (const auto& t_node : tp_value->GetArray()) {
            sgrn::Result<UdtDefinition, scl::SclError> p_udt = SchemaSerializer::udtFromJson(t_node);
            if (p_udt.hasError()) {
                return Error(p_udt.error());
            }
            sgrn::Result<void, scl::SclError> r = t_registry.addUdt(std::move(p_udt.value()));
            if (r.hasError()) {
                return Error(r.error());
            }
        }
        return {};
    };

    auto load_dbs = [&](const rapidjson::Value* tp_value) -> sgrn::Result<void, scl::SclError> {
        if (!tp_value)
            return {};
        if (!tp_value->IsArray())
            return scl::SclError::Generic;
        for (const auto& t_node : tp_value->GetArray()) {
            sgrn::Result<DbSchema, scl::SclError> t_db = SchemaSerializer::dbFromJson(t_node);
            if (t_db.hasError()) {
                return Error(t_db.error());
            }
            if (t_db.value().db_number <= 0 && !t_db.value().db_name.empty())
                t_db.value().db_number = extractTrailingNumber(t_db.value().db_name);
            if (t_db.value().db_number <= 0)
                return scl::SclError::Generic;
            t_registry.dbs_[t_db.value().db_number] = std::move(t_db.value());
        }
        return {};
    };

    auto load_tags = [&](const rapidjson::Value* tp_value) -> sgrn::Result<void, scl::SclError> {
        if (!tp_value)
            return {};
        if (!tp_value->IsArray())
            return scl::SclError::Generic;
        for (const auto& t_node : tp_value->GetArray()) {
            auto t_tag = SchemaSerializer::tagFromJson(t_node);
            if (t_tag.hasError())
                return Error(t_tag.error());
            t_registry.addTag(std::move(t_tag.value()));
        }
        return {};
    };

    if (t_root.IsObject()) {
        sgrn::Result<void, scl::SclError> udt_status = load_udts(t_root.HasMember("udts") ? &t_root["udts"] : nullptr);
        if (udt_status.hasError())
            return udt_status;
        sgrn::Result<void, scl::SclError> db_status = load_dbs(t_root.HasMember("dbs") ? &t_root["dbs"] : nullptr);
        if (db_status.hasError())
            return db_status;
        sgrn::Result<void, scl::SclError> tag_status = load_tags(t_root.HasMember("tags") ? &t_root["tags"] : nullptr);
        if (tag_status.hasError())
            return tag_status;
    } else {
        sgrn::Result<void, scl::SclError> db_status = load_dbs(&t_root);
        if (db_status.hasError())
            return db_status;
    }

    t_registry.rebuildIndices();
    SchemaSerializer::resolveUdtsInRegistry(t_registry);
    t_registry.rebuildIndices();

    return {};
}

void SchemaSerializer::resolveUdtsInRegistry(PlcSchemaStore& t_registry) {
    for (auto& [_, t_db] : t_registry.dbs_) {
        for (auto& t_field : t_db.fields) {
            detail::resolveUdtInField(t_field, t_registry);
        }
    }
    for (auto& p_udt : t_registry.udts_) {
        for (auto& t_field : p_udt.fields) {
            detail::resolveUdtInField(t_field, t_registry);
        }
    }
}

} // namespace sgrn::scl

// ─────────────────────────────────────────────────────────────────────────────
// Binary schema codec (archive header, WAL version 4+)
//
// Layout (all integers little-endian; strings are u32 length + UTF-8 bytes):
//   "SGRS" magic + codec_ver:u16 + flags:u16(reserved)
//   db_count:u32, udt_count:u32, tag_count:u32, then the three sections.
// Field record: name, offset:i32, bit:u8, type name, count:u32,
//   array bounds:i32 x2, string_capacity:u32, struct_size:u32, flags:u16,
//   then optionals in flag order (udt_name, children, unit, min:f64,
//   max:f64, enum_map, init_value, dimension), then, when bit 15 is set,
//   an extended block (ext:u16, then desc, precision:u8, nominal:f64,
//   alarm_lo:f64 + alarm_hi:f64 in ext-bit order). Flags also carry
//   trigger_events, is_dynamic, and the 2-bit endianness (0=Big, 1=Little,
//   2=Unknown). NOTE: records WITH dimension or extended metadata require
//   a reader that knows the new bits; old readers only stay compatible
//   with schemas that don't use them.
// Unlike the JSON form this is lossless: array bounds, init values, UDT
// alias detail, and full tag addresses all survive the round-trip.
// ─────────────────────────────────────────────────────────────────────────────

namespace sgrn::scl
{
namespace bin
{

// Field presence/property flags (u16).
inline constexpr uint16_t kHasUdt = 1 << 0;
inline constexpr uint16_t kHasChildren = 1 << 1;
inline constexpr uint16_t kHasUnit = 1 << 2;
inline constexpr uint16_t kHasMin = 1 << 3;
inline constexpr uint16_t kHasMax = 1 << 4;
inline constexpr uint16_t kHasEnum = 1 << 5;
inline constexpr uint16_t kHasInit = 1 << 6;
inline constexpr uint16_t kTriggerEvents = 1 << 7;
inline constexpr uint16_t kIsDynamic = 1 << 8;
inline constexpr uint16_t kHasDimension = 1 << 9;
// Bit 15: extended display metadata block (u16 ext flags + optionals).
// Keeps the base record shape stable; readers that predate the block must
// refuse records with this bit (same one-way note as dimension).
inline constexpr uint16_t kHasExt = 1 << 15;
// Extended block bits (u16, written only when kHasExt is set).
inline constexpr uint16_t kExtHasDesc = 1 << 0;
inline constexpr uint16_t kExtHasPrecision = 1 << 1;
inline constexpr uint16_t kExtHasNominal = 1 << 2;
inline constexpr uint16_t kExtHasAlarm = 1 << 3;
inline constexpr uint16_t kExtIsLabel = 1 << 4;
inline constexpr uint16_t kExtIsTransient = 1 << 5;
inline constexpr uint16_t kExtIsReadOnly = 1 << 6;
// Bits 10-11: endianness (0 = Big default, 1 = Little, 2 = Unknown).
inline constexpr uint16_t kEndianShift = 10;
inline constexpr uint16_t kEndianMask = 0x3 << kEndianShift;

// DB flags (u8): bit0 = source_file present.
inline constexpr uint8_t kDbHasSourceFile = 1 << 0;
// UDT flags (u8): bit0 = enum_map, bit1 = unit, bit2 = min, bit3 = max,
// bit4 = dimension, bit7 = extended display block (u8 ext + desc,
// precision:u8, nominal:f64, alarm_lo/hi:f64; same bit layout as the field
// extended block). Appended after max, before the field count.
inline constexpr uint8_t kUdtHasEnum = 1 << 0;
inline constexpr uint8_t kUdtHasUnit = 1 << 1;
inline constexpr uint8_t kUdtHasMin = 1 << 2;
inline constexpr uint8_t kUdtHasMax = 1 << 3;
inline constexpr uint8_t kUdtHasDimension = 1 << 4;
inline constexpr uint8_t kUdtHasExt = 1 << 7;

inline constexpr size_t kMaxStringLen = 1u << 24;
inline constexpr size_t kMaxCount = 1u << 20;

class Writer {
public:
    void u8(uint8_t t_v) {
        buf_.push_back(static_cast<char>(t_v));
    }
    void u16(uint16_t t_v) {
        for (int i = 0; i < 2; ++i)
            buf_.push_back(static_cast<char>((t_v >> (8 * i)) & 0xFF));
    }
    void u32(uint32_t t_v) {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<char>((t_v >> (8 * i)) & 0xFF));
    }
    void i32(int32_t t_v) {
        u32(static_cast<uint32_t>(t_v));
    }
    void f64(double t_v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &t_v, sizeof(bits));
        for (int i = 0; i < 8; ++i)
            buf_.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
    }
    void str(const std::string& t_s) {
        u32(static_cast<uint32_t>(t_s.size()));
        buf_.append(t_s);
    }
    std::string finish() {
        return std::move(buf_);
    }

private:
    std::string buf_;
};

class Reader {
public:
    explicit Reader(std::string_view t_bytes)
        : bytes_(t_bytes) {
    }
    bool u8(uint8_t& t_v) {
        if (pos_ + 1 > bytes_.size())
            return false;
        t_v = static_cast<uint8_t>(bytes_[pos_]);
        pos_ += 1;
        return true;
    }
    bool u16(uint16_t& t_v) {
        if (pos_ + 2 > bytes_.size())
            return false;
        t_v = static_cast<uint16_t>(static_cast<uint8_t>(bytes_[pos_]) | (static_cast<uint8_t>(bytes_[pos_ + 1]) << 8));
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t& t_v) {
        if (pos_ + 4 > bytes_.size())
            return false;
        t_v = 0;
        for (int i = 0; i < 4; ++i)
            t_v |= static_cast<uint32_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
        pos_ += 4;
        return true;
    }
    bool i32(int32_t& t_v) {
        uint32_t u = 0;
        if (!u32(u))
            return false;
        std::memcpy(&t_v, &u, sizeof(t_v));
        return true;
    }
    bool f64(double& t_v) {
        if (pos_ + 8 > bytes_.size())
            return false;
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
        std::memcpy(&t_v, &bits, sizeof(t_v));
        pos_ += 8;
        return true;
    }
    bool str(std::string& t_s) {
        uint32_t len = 0;
        if (!u32(len) || len > kMaxStringLen || pos_ + len > bytes_.size())
            return false;
        t_s.assign(bytes_.data() + pos_, len);
        pos_ += len;
        return true;
    }
    bool atEnd() const {
        return pos_ == bytes_.size();
    }

private:
    std::string_view bytes_;
    size_t pos_{0};
};

inline uint8_t endianToU8(s7codec::Endian t_e) {
    switch (t_e) {
        case s7codec::Endian::Little:
            return 1;
        case s7codec::Endian::Unknown:
            return 2;
        default:
            return 0;
    }
}

inline bool u8ToEndian(uint8_t t_v, s7codec::Endian& t_e) {
    switch (t_v) {
        case 0:
            t_e = s7codec::Endian::Big;
            return true;
        case 1:
            t_e = s7codec::Endian::Little;
            return true;
        case 2:
            t_e = s7codec::Endian::Unknown;
            return true;
        default:
            return false;
    }
}

void writeField(Writer& t_w, const DbField& t_f) {
    t_w.str(t_f.name);
    t_w.i32(t_f.offset);
    t_w.u8(t_f.bit_index);
    t_w.str(s7codec::s7TypeToString(t_f.type));
    t_w.u32(t_f.count);
    t_w.i32(t_f.array_lower_bound);
    t_w.i32(t_f.array_upper_bound);
    t_w.u32(t_f.string_capacity);
    t_w.u32(t_f.struct_size);

    uint16_t flags = 0;
    if (!t_f.udt_name.empty())
        flags |= kHasUdt;
    if (!t_f.children.empty())
        flags |= kHasChildren;
    if (t_f.unit.has_value())
        flags |= kHasUnit;
    if (t_f.min_val.has_value())
        flags |= kHasMin;
    if (t_f.max_val.has_value())
        flags |= kHasMax;
    if (!t_f.enum_map.empty())
        flags |= kHasEnum;
    if (!t_f.init_value.empty())
        flags |= kHasInit;
    if (t_f.trigger_events)
        flags |= kTriggerEvents;
    if (t_f.is_dynamic)
        flags |= kIsDynamic;
    if (t_f.dimension.has_value())
        flags |= kHasDimension;
    const bool has_ext = t_f.description.has_value() || t_f.precision.has_value() || t_f.nominal.has_value() ||
                         (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value()) || t_f.is_label || t_f.is_transient || t_f.is_read_only;
    if (has_ext)
        flags |= kHasExt;
    flags |= static_cast<uint16_t>(endianToU8(t_f.endianness) << kEndianShift);
    t_w.u16(flags);

    if (!t_f.udt_name.empty())
        t_w.str(t_f.udt_name);
    if (!t_f.children.empty()) {
        t_w.u32(static_cast<uint32_t>(t_f.children.size()));
        for (const auto& child : t_f.children)
            writeField(t_w, child);
    }
    if (t_f.unit.has_value())
        t_w.str(t_f.unit.value());
    if (t_f.min_val.has_value())
        t_w.f64(t_f.min_val.value());
    if (t_f.max_val.has_value())
        t_w.f64(t_f.max_val.value());
    if (!t_f.enum_map.empty()) {
        t_w.u32(static_cast<uint32_t>(t_f.enum_map.size()));
        for (const auto& [key, value] : t_f.enum_map) {
            t_w.i32(key);
            t_w.str(value);
        }
    }
    if (!t_f.init_value.empty())
        t_w.str(t_f.init_value);
    if (t_f.dimension.has_value())
        t_w.str(t_f.dimension.value());
    if (has_ext) {
        uint16_t ext = 0;
        if (t_f.description.has_value())
            ext |= kExtHasDesc;
        if (t_f.precision.has_value())
            ext |= kExtHasPrecision;
        if (t_f.nominal.has_value())
            ext |= kExtHasNominal;
        if (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value())
            ext |= kExtHasAlarm;
        if (t_f.is_label)
            ext |= kExtIsLabel;
        if (t_f.is_transient)
            ext |= kExtIsTransient;
        if (t_f.is_read_only)
            ext |= kExtIsReadOnly;
        t_w.u16(ext);
        if (t_f.description.has_value())
            t_w.str(t_f.description.value());
        if (t_f.precision.has_value())
            t_w.u8(static_cast<uint8_t>(std::clamp(t_f.precision.value(), 0, 255)));
        if (t_f.nominal.has_value())
            t_w.f64(t_f.nominal.value());
        if (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value()) {
            t_w.f64(t_f.alarm_lo.value());
            t_w.f64(t_f.alarm_hi.value());
        }
    }
}

bool readField(Reader& t_r, DbField& t_f, int t_depth = 0) {
    // Schema trees are shallow; cap recursion against malicious payloads.
    if (t_depth > 64)
        return false;
    uint32_t count = 0;
    uint16_t flags = 0;
    std::string type_name;
    if (!t_r.str(t_f.name) || !t_r.i32(t_f.offset) || !t_r.u8(t_f.bit_index) || !t_r.str(type_name) || !t_r.u32(count) ||
        !t_r.i32(t_f.array_lower_bound) || !t_r.i32(t_f.array_upper_bound) || !t_r.u32(t_f.string_capacity) || !t_r.u32(t_f.struct_size) ||
        !t_r.u16(flags))
        return false;
    if (count > kMaxCount)
        return false;
    t_f.count = count;
    s7codec::Type type = s7codec::Type::Byte;
    if (!s7codec::stringToType(type_name.c_str(), type))
        return false;
    t_f.type = type;
    s7codec::Endian endian = s7codec::Endian::Big;
    if (!u8ToEndian(static_cast<uint8_t>((flags & kEndianMask) >> kEndianShift), endian))
        return false;
    t_f.endianness = endian;
    t_f.trigger_events = (flags & kTriggerEvents) != 0;
    t_f.is_dynamic = (flags & kIsDynamic) != 0;

    if (flags & kHasUdt) {
        if (!t_r.str(t_f.udt_name))
            return false;
    }
    if (flags & kHasChildren) {
        uint32_t n = 0;
        if (!t_r.u32(n) || n > kMaxCount)
            return false;
        t_f.children.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            DbField child;
            if (!readField(t_r, child, t_depth + 1))
                return false;
            t_f.children.push_back(std::move(child));
        }
    }
    if (flags & kHasUnit) {
        std::string unit;
        if (!t_r.str(unit))
            return false;
        t_f.unit = std::move(unit);
    }
    if (flags & kHasMin) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_f.min_val = v;
    }
    if (flags & kHasMax) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_f.max_val = v;
    }
    if (flags & kHasEnum) {
        uint32_t n = 0;
        if (!t_r.u32(n) || n > kMaxCount)
            return false;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t key = 0;
            std::string value;
            if (!t_r.i32(key) || !t_r.str(value))
                return false;
            t_f.enum_map[key] = std::move(value);
        }
    }
    if (flags & kHasInit) {
        if (!t_r.str(t_f.init_value))
            return false;
    }
    if (flags & kHasDimension) {
        std::string dimension;
        if (!t_r.str(dimension))
            return false;
        t_f.dimension = std::move(dimension);
    }
    if (flags & kHasExt) {
        uint16_t ext = 0;
        if (!t_r.u16(ext))
            return false;
        if (ext & kExtHasDesc) {
            std::string description;
            if (!t_r.str(description))
                return false;
            t_f.description = std::move(description);
        }
        if (ext & kExtHasPrecision) {
            uint8_t precision = 0;
            if (!t_r.u8(precision))
                return false;
            t_f.precision = static_cast<int>(precision);
        }
        if (ext & kExtHasNominal) {
            double nominal = 0.0;
            if (!t_r.f64(nominal))
                return false;
            t_f.nominal = nominal;
        }
        if (ext & kExtHasAlarm) {
            double lo = 0.0, hi = 0.0;
            if (!t_r.f64(lo) || !t_r.f64(hi))
                return false;
            t_f.alarm_lo = lo;
            t_f.alarm_hi = hi;
        }
        t_f.is_label = (ext & kExtIsLabel) != 0;
        t_f.is_transient = (ext & kExtIsTransient) != 0;
        t_f.is_read_only = (ext & kExtIsReadOnly) != 0;
    }
    return true;
}

void writeDb(Writer& t_w, const DbSchema& t_db) {
    t_w.u16(t_db.db_number);
    t_w.str(t_db.db_name);
    t_w.i32(t_db.size_bytes);
    t_w.i32(t_db.max_depth);
    t_w.u8(endianToU8(t_db.endianness));
    t_w.u8(t_db.trigger_events ? 1 : 0);
    t_w.u8(static_cast<uint8_t>(t_db.modbus_area));
    uint8_t flags = t_db.source_file.empty() ? 0 : kDbHasSourceFile;
    t_w.u8(flags);
    if (!t_db.source_file.empty())
        t_w.str(t_db.source_file);
    t_w.u32(static_cast<uint32_t>(t_db.fields.size()));
    for (const auto& field : t_db.fields)
        writeField(t_w, field);
}

bool readDb(Reader& t_r, DbSchema& t_db) {
    uint16_t number = 0;
    uint8_t endian = 0;
    uint8_t trigger = 0;
    uint8_t area = 0;
    uint8_t flags = 0;
    if (!t_r.u16(number) || !t_r.str(t_db.db_name) || !t_r.i32(t_db.size_bytes) || !t_r.i32(t_db.max_depth) || !t_r.u8(endian) ||
        !t_r.u8(trigger) || !t_r.u8(area) || !t_r.u8(flags))
        return false;
    if (!u8ToEndian(endian, t_db.endianness) || area > static_cast<uint8_t>(ModbusArea::Discrete))
        return false;
    t_db.db_number = number;
    t_db.trigger_events = trigger != 0;
    t_db.modbus_area = static_cast<ModbusArea>(area);
    // NOTE: source_file precedes the field count in the byte order.
    if (flags & kDbHasSourceFile) {
        if (!t_r.str(t_db.source_file))
            return false;
    }
    uint32_t n = 0;
    if (!t_r.u32(n) || n > kMaxCount)
        return false;
    t_db.fields.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        DbField field;
        if (!readField(t_r, field))
            return false;
        t_db.fields.push_back(std::move(field));
    }
    return true;
}

void writeUdt(Writer& t_w, const UdtDefinition& t_udt) {
    t_w.u16(t_udt.udt_number);
    t_w.str(t_udt.name);
    t_w.i32(t_udt.size_bytes);
    t_w.i32(t_udt.max_depth);
    t_w.u8(endianToU8(t_udt.endianness));
    t_w.u8(t_udt.trigger_events ? 1 : 0);
    t_w.u8(t_udt.is_scalar_alias ? 1 : 0);
    t_w.str(s7codec::s7TypeToString(t_udt.scalar_type));
    uint8_t flags = 0;
    if (!t_udt.enum_map.empty())
        flags |= kUdtHasEnum;
    if (t_udt.unit.has_value())
        flags |= kUdtHasUnit;
    if (t_udt.min_val.has_value())
        flags |= kUdtHasMin;
    if (t_udt.max_val.has_value())
        flags |= kUdtHasMax;
    if (t_udt.dimension.has_value())
        flags |= kUdtHasDimension;
    const bool udt_has_ext = t_udt.description.has_value() || t_udt.precision.has_value() || t_udt.nominal.has_value() ||
                             (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value());
    if (udt_has_ext)
        flags |= kUdtHasExt;
    t_w.u8(flags);
    if (!t_udt.enum_map.empty()) {
        t_w.u32(static_cast<uint32_t>(t_udt.enum_map.size()));
        for (const auto& [key, value] : t_udt.enum_map) {
            t_w.i32(key);
            t_w.str(value);
        }
    }
    if (t_udt.unit.has_value())
        t_w.str(t_udt.unit.value());
    if (t_udt.min_val.has_value())
        t_w.f64(t_udt.min_val.value());
    if (t_udt.max_val.has_value())
        t_w.f64(t_udt.max_val.value());
    if (t_udt.dimension.has_value())
        t_w.str(t_udt.dimension.value());
    if (udt_has_ext) {
        // Same bit layout as the field extended block (kExt* constants).
        uint16_t ext = 0;
        if (t_udt.description.has_value())
            ext |= kExtHasDesc;
        if (t_udt.precision.has_value())
            ext |= kExtHasPrecision;
        if (t_udt.nominal.has_value())
            ext |= kExtHasNominal;
        if (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value())
            ext |= kExtHasAlarm;
        t_w.u16(ext);
        if (t_udt.description.has_value())
            t_w.str(t_udt.description.value());
        if (t_udt.precision.has_value())
            t_w.u8(static_cast<uint8_t>(std::clamp(t_udt.precision.value(), 0, 255)));
        if (t_udt.nominal.has_value())
            t_w.f64(t_udt.nominal.value());
        if (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value()) {
            t_w.f64(t_udt.alarm_lo.value());
            t_w.f64(t_udt.alarm_hi.value());
        }
    }
    t_w.u32(static_cast<uint32_t>(t_udt.fields.size()));
    for (const auto& field : t_udt.fields)
        writeField(t_w, field);
}

bool readUdt(Reader& t_r, UdtDefinition& t_udt) {
    uint16_t number = 0;
    uint8_t endian = 0;
    uint8_t trigger = 0;
    uint8_t alias = 0;
    uint8_t flags = 0;
    std::string scalar_name;
    if (!t_r.u16(number) || !t_r.str(t_udt.name) || !t_r.i32(t_udt.size_bytes) || !t_r.i32(t_udt.max_depth) || !t_r.u8(endian) ||
        !t_r.u8(trigger) || !t_r.u8(alias) || !t_r.str(scalar_name) || !t_r.u8(flags))
        return false;
    if (!u8ToEndian(endian, t_udt.endianness))
        return false;
    t_udt.udt_number = number;
    t_udt.trigger_events = trigger != 0;
    t_udt.is_scalar_alias = alias != 0;
    if (!s7codec::stringToType(scalar_name.c_str(), t_udt.scalar_type))
        return false;
    if (flags & kUdtHasEnum) {
        uint32_t m = 0;
        if (!t_r.u32(m) || m > kMaxCount)
            return false;
        for (uint32_t i = 0; i < m; ++i) {
            int32_t key = 0;
            std::string value;
            if (!t_r.i32(key) || !t_r.str(value))
                return false;
            t_udt.enum_map[key] = std::move(value);
        }
    }
    if (flags & kUdtHasUnit) {
        std::string unit;
        if (!t_r.str(unit))
            return false;
        t_udt.unit = std::move(unit);
    }
    if (flags & kUdtHasMin) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_udt.min_val = v;
    }
    if (flags & kUdtHasMax) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_udt.max_val = v;
    }
    if (flags & kUdtHasDimension) {
        std::string dimension;
        if (!t_r.str(dimension))
            return false;
        t_udt.dimension = std::move(dimension);
    }
    if (flags & kUdtHasExt) {
        uint16_t ext = 0;
        if (!t_r.u16(ext))
            return false;
        if (ext & kExtHasDesc) {
            std::string description;
            if (!t_r.str(description))
                return false;
            t_udt.description = std::move(description);
        }
        if (ext & kExtHasPrecision) {
            uint8_t precision = 0;
            if (!t_r.u8(precision))
                return false;
            t_udt.precision = static_cast<int>(precision);
        }
        if (ext & kExtHasNominal) {
            double nominal = 0.0;
            if (!t_r.f64(nominal))
                return false;
            t_udt.nominal = nominal;
        }
        if (ext & kExtHasAlarm) {
            double lo = 0.0, hi = 0.0;
            if (!t_r.f64(lo) || !t_r.f64(hi))
                return false;
            t_udt.alarm_lo = lo;
            t_udt.alarm_hi = hi;
        }
    }
    // Fields come last so a truncated payload fails before mutating much.
    // (The store itself is only committed by the caller on full success.)
    uint32_t n = 0;
    if (!t_r.u32(n) || n > kMaxCount)
        return false;
    std::vector<DbField> fields;
    fields.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        DbField field;
        if (!readField(t_r, field))
            return false;
        fields.push_back(std::move(field));
    }
    t_udt.fields = std::move(fields);
    return true;
}

void writeTag(Writer& t_w, const PlcTag& t_tag) {
    t_w.str(t_tag.name);
    t_w.str(t_tag.table_name);
    t_w.str(t_tag.type_str);
    t_w.str(t_tag.remark);
    t_w.i32(t_tag.addr.area);
    t_w.u16(t_tag.addr.db_number);
    t_w.i32(t_tag.addr.byte_offset);
    t_w.i32(t_tag.addr.bit_index);
    t_w.i32(t_tag.addr.word_len);
    t_w.i32(t_tag.addr.byte_count);
    t_w.str(t_tag.addr.label);
    t_w.str(s7codec::s7TypeToString(t_tag.type));
}

bool readTag(Reader& t_r, PlcTag& t_tag) {
    int32_t area = 0;
    int32_t byte_offset = 0;
    int32_t bit_index = 0;
    int32_t word_len = 0;
    int32_t byte_count = 0;
    uint16_t db_number = 0;
    std::string type_name;
    if (!t_r.str(t_tag.name) || !t_r.str(t_tag.table_name) || !t_r.str(t_tag.type_str) || !t_r.str(t_tag.remark) || !t_r.i32(area) ||
        !t_r.u16(db_number) || !t_r.i32(byte_offset) || !t_r.i32(bit_index) || !t_r.i32(word_len) || !t_r.i32(byte_count) ||
        !t_r.str(t_tag.addr.label) || !t_r.str(type_name))
        return false;
    t_tag.addr.area = area;
    t_tag.addr.db_number = db_number;
    t_tag.addr.byte_offset = byte_offset;
    t_tag.addr.bit_index = bit_index;
    t_tag.addr.word_len = word_len;
    t_tag.addr.byte_count = byte_count;
    if (!s7codec::stringToType(type_name.c_str(), t_tag.type))
        return false;
    return true;
}

} // namespace bin

sgrn::Result<std::string, scl::SclError> SchemaSerializer::serializeBinary(const PlcSchemaStore& t_registry) {
    bin::Writer w;
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[0]));
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[1]));
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[2]));
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[3]));
    w.u16(kBinarySchemaCodecVersion);
    w.u16(0); // flags, reserved
    w.u32(static_cast<uint32_t>(t_registry.dbs().size()));
    w.u32(static_cast<uint32_t>(t_registry.udts().size()));
    w.u32(static_cast<uint32_t>(t_registry.tags().size()));
    for (const auto& [num, db] : t_registry.dbs()) {
        (void)num;
        bin::writeDb(w, db);
    }
    for (const auto& udt : t_registry.udts())
        bin::writeUdt(w, udt);
    for (const auto& [name, tag] : t_registry.tags()) {
        (void)name;
        bin::writeTag(w, tag);
    }
    return w.finish();
}

sgrn::Result<void, scl::SclError> SchemaSerializer::deserializeBinary(PlcSchemaStore& t_registry, std::string_view t_bytes) {
    bin::Reader r(t_bytes);
    for (char c : kBinarySchemaMagic) {
        uint8_t v = 0;
        if (!r.u8(v) || v != static_cast<uint8_t>(c))
            return scl::SclError::ParseError;
    }
    uint16_t codec_ver = 0;
    uint16_t flags = 0;
    uint32_t db_count = 0;
    uint32_t udt_count = 0;
    uint32_t tag_count = 0;
    if (!r.u16(codec_ver) || !r.u16(flags) || !r.u32(db_count) || !r.u32(udt_count) || !r.u32(tag_count))
        return scl::SclError::ParseError;
    if (codec_ver != kBinarySchemaCodecVersion)
        return scl::SclError::UnsupportedType;
    if (flags != 0)
        return scl::SclError::UnsupportedType;
    if (db_count > bin::kMaxCount || udt_count > bin::kMaxCount || tag_count > bin::kMaxCount)
        return scl::SclError::OutOfRange;

    // Parse into locals first; the registry is only committed below, mirroring
    // deserialize()'s structure (direct dbs_ insert, validated UDT/tag adds).
    std::vector<DbSchema> dbs;
    dbs.reserve(db_count);
    for (uint32_t i = 0; i < db_count; ++i) {
        DbSchema db;
        if (!bin::readDb(r, db))
            return scl::SclError::ParseError;
        dbs.push_back(std::move(db));
    }
    std::vector<UdtDefinition> udts;
    udts.reserve(udt_count);
    for (uint32_t i = 0; i < udt_count; ++i) {
        UdtDefinition udt;
        if (!bin::readUdt(r, udt))
            return scl::SclError::ParseError;
        udts.push_back(std::move(udt));
    }
    std::vector<PlcTag> tags;
    tags.reserve(tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        PlcTag tag;
        if (!bin::readTag(r, tag))
            return scl::SclError::ParseError;
        tags.push_back(std::move(tag));
    }
    if (!r.atEnd())
        return scl::SclError::ParseError;

    for (auto& db : dbs)
        t_registry.dbs_[db.db_number] = std::move(db);
    for (auto& udt : udts) {
        if (auto res = t_registry.addUdt(std::move(udt)); res.hasError())
            return res.error();
    }
    for (auto& tag : tags) {
        if (auto res = t_registry.addTag(std::move(tag)); res.hasError())
            return res.error();
    }

    t_registry.rebuildIndices();
    SchemaSerializer::resolveUdtsInRegistry(t_registry);
    t_registry.rebuildIndices();
    return {};
}

} // namespace sgrn::scl
