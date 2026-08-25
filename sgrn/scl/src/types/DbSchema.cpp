#include <sgrn/scl/types/DbSchema.hpp>

#include <algorithm>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::DbSchema& t_db) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    sgrn::scl::db::serializeToWriter(t_writer, t_db);
    return sb.GetString();
}

rapidjson::Document toJson(const sgrn::scl::DbSchema& t_db) {
    rapidjson::Document doc;
    doc.Parse(toJsonString(t_db).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl
{

namespace
{
int fieldSpanBytes(const sgrn::scl::DbField& t_field) {
    if (t_field.type == sgrn::scl::DataType::Struct)
        return std::max(1, t_field.struct_size) * std::max(1, t_field.count);
    return s7codec::typeSpanBytes(t_field.type, t_field.count);
}
} // namespace

sgrn::scl::DbSchema fromJson(const rapidjson::Value& t_node) {
    sgrn::scl::DbSchema t_db;
    if (!t_node.IsObject())
        return t_db;

    if (t_node.HasMember("db_number"))
        t_db.db_number = t_node["db_number"].GetUint();
    if (t_node.HasMember("db_name") && t_node["db_name"].IsString())
        t_db.db_name = t_node["db_name"].GetString();
    if (t_node.HasMember("size_bytes"))
        t_db.size_bytes = t_node["size_bytes"].GetInt();
    if (t_node.HasMember("source_file") && t_node["source_file"].IsString())
        t_db.source_file = t_node["source_file"].GetString();
    if (t_node.HasMember("endianness") && t_node["endianness"].IsString()) {
        const std::string e = t_node["endianness"].GetString();
        t_db.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.HasMember("trigger_events") && t_node["trigger_events"].IsBool())
        t_db.trigger_events = t_node["trigger_events"].GetBool();
    if (t_node.HasMember("modbus_area") && t_node["modbus_area"].IsString()) {
        const std::string a = t_node["modbus_area"].GetString();
        if (a == "holding")
            t_db.modbus_area = sgrn::scl::ModbusArea::Holding;
        else if (a == "input")
            t_db.modbus_area = sgrn::scl::ModbusArea::Input;
        else if (a == "coil")
            t_db.modbus_area = sgrn::scl::ModbusArea::Coil;
        else if (a == "discrete")
            t_db.modbus_area = sgrn::scl::ModbusArea::Discrete;
    }

    if (t_node.HasMember("fields") && t_node["fields"].IsArray()) {
        for (const auto& field_node : t_node["fields"].GetArray()) {
            t_db.fields.push_back(sgrn::scl::field::fromJson(field_node));
        }
    }

    if (t_db.size_bytes == 0) {
        int max_end = 0;
        for (const auto& t_field : t_db.fields)
            max_end = std::max(max_end, t_field.offset + fieldSpanBytes(t_field));
        t_db.size_bytes = max_end;
    }

    return t_db;
}

sgrn::scl::DbSchema fromJsonString(const std::string& t_value) {
    rapidjson::Document doc;
    doc.Parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl
