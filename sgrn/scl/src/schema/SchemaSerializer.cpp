#include <fmt/core.h>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/utils.hpp>
#include <algorithm>
#include <regex>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sgrn::scl
{

namespace detail
{

static int fieldSpanBytes(const DbField& t_field) {
    if (t_field.type == DataType::Struct)
        return std::max(1, t_field.struct_size) * std::max(1, t_field.count);
    return s7codec::typeSpanBytes(t_field.type, t_field.count);
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
    t_writer.Key("bit_index");
    t_writer.Int(t_field.bit_index);
    t_writer.Key("type");
    t_writer.String(s7codec::s7TypeToString(t_field.type));

    if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
        t_field.type == DataType::XWString) {
        if (t_field.struct_size > 0) { // Array of strings
            t_writer.Key("count");
            t_writer.Int(t_field.count);
            t_writer.Key("capacity");
            t_writer.Int(t_field.struct_size);
        } else { // Scalar string
            t_writer.Key("count");
            t_writer.Int(1);
            t_writer.Key("capacity");
            t_writer.Int(t_field.count);
        }
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
    t_writer.EndObject();
}

template <typename Writer>
static void serializeDataBlockRegistry(Writer& t_writer, const DataBlockRegistry& t_db, bool t_headers_only) {
    t_writer.StartObject();
    t_writer.Key("db_number");
    t_writer.Int(t_db.db_number);
    t_writer.Key("db_name");
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

        serializeDataBlockRegistry(t_writer, t_db, t_headers_only);

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

std::string SchemaSerializer::dbToJson(const DataBlockRegistry& t_db) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    detail::serializeDataBlockRegistry(t_writer, t_db, false);
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

static sgrn::Result<DbField, ::sgrn::scl::Error> fieldFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return Error{SchemaCode::Generic, "field entry must be an object"};

    DbField t_field;
    if (!t_node.HasMember("name") || !t_node["name"].IsString())
        return Error{SchemaCode::Generic, "field entry missing string 'name'"};
    t_field.name = t_node["name"].GetString();

    if (t_node.HasMember("offset"))
        t_field.offset = t_node["offset"].GetInt();
    if (t_node.HasMember("bit_index"))
        t_field.bit_index = t_node["bit_index"].GetInt();
    if (t_node.HasMember("count"))
        t_field.count = t_node["count"].GetInt();
    if (t_node.HasMember("udt_name") && t_node["udt_name"].IsString())
        t_field.udt_name = t_node["udt_name"].GetString();
    if (t_node.HasMember("struct_size"))
        t_field.struct_size = t_node["struct_size"].GetInt();
    if (t_node.HasMember("unit") && t_node["unit"].IsString())
        t_field.unit = t_node["unit"].GetString();
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
        return Err::Generic("field '{}' missing string 'type'", t_field.name);

    std::optional<DataType> type = parseDataType(t_node["type"].GetString());
    if (!type.has_value()) {
        t_field.type = DataType::Struct;
        t_field.udt_name = t_node["type"].GetString();
    } else {
        t_field.type = type.value();
    }

    if (t_node.HasMember("children")) {
        if (!t_node["children"].IsArray())
            return Err::Generic("field '{}' children must be an array", t_field.name);
        for (const auto& child_node : t_node["children"].GetArray()) {
            sgrn::Result<DbField, ::sgrn::scl::Error> child = fieldFromJson(child_node);
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

sgrn::Result<UdtDefinition, ::sgrn::scl::Error> SchemaSerializer::udtFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return Error{SchemaCode::Generic, "UDT entry must be an object"};

    UdtDefinition p_udt;
    if (t_node.HasMember("udt_number"))
        p_udt.udt_number = t_node["udt_number"].GetInt();
    if (t_node.HasMember("name") && t_node["name"].IsString())
        p_udt.name = t_node["name"].GetString();
    if (t_node.HasMember("size_bytes"))
        p_udt.size_bytes = t_node["size_bytes"].GetInt();

    if (t_node.HasMember("fields")) {
        if (!t_node["fields"].IsArray())
            return Error{SchemaCode::Generic, "UDT 'fields' must be an array"};
        for (const auto& field_node : t_node["fields"].GetArray()) {
            sgrn::Result<DbField, ::sgrn::scl::Error> t_field = fieldFromJson(field_node);
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

    return p_udt;
}

sgrn::Result<DataBlockRegistry, ::sgrn::scl::Error> SchemaSerializer::dbFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return Error{SchemaCode::Generic, "DB entry must be an object"};

    DataBlockRegistry t_db;
    if (t_node.HasMember("db_number"))
        t_db.db_number = t_node["db_number"].GetInt();
    if (t_node.HasMember("db_name") && t_node["db_name"].IsString())
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
            return Error{SchemaCode::Generic, "DB 'fields' must be an array"};
        for (const auto& field_node : t_node["fields"].GetArray()) {
            sgrn::Result<DbField, ::sgrn::scl::Error> t_field = fieldFromJson(field_node);
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

sgrn::Result<PlcTag, ::sgrn::scl::Error> SchemaSerializer::tagFromJson(const rapidjson::Value& t_node) {
    if (!t_node.IsObject())
        return Error{SchemaCode::Generic, "tag entry must be an object"};
    PlcTag t_tag;
    if (!t_node.HasMember("name") || !t_node["name"].IsString())
        return Error{SchemaCode::Generic, "tag entry missing string 'name'"};
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
            return Error{SchemaCode::Generic, fmt::format("tag '{}' has invalid address '{}'", t_tag.name, t_node["address"].GetString())};
    }
    return t_tag;
}

sgrn::Result<void, ::sgrn::scl::Error> SchemaSerializer::deserialize(PlcSchemaStore& t_registry, const rapidjson::Value& t_root) {
    if (!t_root.IsObject() && !t_root.IsArray())
        return Error{SchemaCode::Generic, "JSON registry root must be an object or array"};

    auto load_udts = [&](const rapidjson::Value* tp_value) -> sgrn::Result<void, ::sgrn::scl::Error> {
        if (!tp_value)
            return {};
        if (!tp_value->IsArray())
            return Error{SchemaCode::Generic, "'udts' must be an array"};
        for (const auto& t_node : tp_value->GetArray()) {
            sgrn::Result<UdtDefinition, ::sgrn::scl::Error> p_udt = SchemaSerializer::udtFromJson(t_node);
            if (p_udt.hasError()) {
                return Error(p_udt.error());
            }
            sgrn::Result<void, ::sgrn::scl::Error> r = t_registry.addUdt(std::move(p_udt.value()));
            if (r.hasError()) {
                return Error(r.error());
            }
        }
        return {};
    };

    auto load_dbs = [&](const rapidjson::Value* tp_value) -> sgrn::Result<void, ::sgrn::scl::Error> {
        if (!tp_value)
            return {};
        if (!tp_value->IsArray())
            return Error{SchemaCode::Generic, "'dbs' must be an array"};
        for (const auto& t_node : tp_value->GetArray()) {
            sgrn::Result<DataBlockRegistry, ::sgrn::scl::Error> t_db = SchemaSerializer::dbFromJson(t_node);
            if (t_db.hasError()) {
                return Error(t_db.error());
            }
            if (t_db.value().db_number <= 0 && !t_db.value().db_name.empty())
                t_db.value().db_number = extractTrailingNumber(t_db.value().db_name);
            if (t_db.value().db_number <= 0)
                return Error{SchemaCode::Generic, "DB entry missing db_number"};
            t_registry.dbs_[t_db.value().db_number] = std::move(t_db.value());
        }
        return {};
    };

    auto load_tags = [&](const rapidjson::Value* tp_value) -> sgrn::Result<void, ::sgrn::scl::Error> {
        if (!tp_value)
            return {};
        if (!tp_value->IsArray())
            return Error{SchemaCode::Generic, "'tags' must be an array"};
        for (const auto& t_node : tp_value->GetArray()) {
            auto t_tag = SchemaSerializer::tagFromJson(t_node);
            if (t_tag.hasError())
                return Error(t_tag.error());
            t_registry.addTag(std::move(t_tag.value()));
        }
        return {};
    };

    if (t_root.IsObject()) {
        sgrn::Result<void, ::sgrn::scl::Error> udt_status = load_udts(t_root.HasMember("udts") ? &t_root["udts"] : nullptr);
        if (udt_status.hasError())
            return udt_status;
        sgrn::Result<void, ::sgrn::scl::Error> db_status = load_dbs(t_root.HasMember("dbs") ? &t_root["dbs"] : nullptr);
        if (db_status.hasError())
            return db_status;
        sgrn::Result<void, ::sgrn::scl::Error> tag_status = load_tags(t_root.HasMember("tags") ? &t_root["tags"] : nullptr);
        if (tag_status.hasError())
            return tag_status;
    } else {
        sgrn::Result<void, ::sgrn::scl::Error> db_status = load_dbs(&t_root);
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
