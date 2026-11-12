#include <sgrn/scl/types/DbSchema.hpp>
#include <sgrn/utils/json-dom.hpp>

#include <algorithm>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::DbSchema& t_db) {
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> t_writer(sb);
    sgrn::scl::db::serializeToWriter(t_writer, t_db);
    return sb.getString();
}

sgrn::json::dom::Document toJson(const sgrn::scl::DbSchema& t_db) {
    sgrn::json::dom::Document doc;
    doc.parse(toJsonString(t_db).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::db
{

namespace
{
int fieldSpanBytes(const sgrn::scl::DbField& t_field) {
    if (t_field.type == sgrn::scl::DataType::Struct)
        return static_cast<int>(std::max(1u, t_field.struct_size) * std::max(1u, t_field.count));
    return static_cast<int>(s7codec::typeSpanBytes(t_field.type, t_field.count).value_or(0));
}
} // namespace

sgrn::scl::DbSchema fromJson(const sgrn::json::dom::Value& t_node) {
    sgrn::scl::DbSchema t_db;
    if (!t_node.is_object())
        return t_db;

    if (t_node.hasMember("number"))
        t_db.db_number = t_node["number"].getUint();
    else if (t_node.hasMember("db_number"))
        t_db.db_number = t_node["db_number"].getUint();
    if (t_node.hasMember("db_name") && t_node["db_name"].is_string())
        t_db.db_name = t_node["db_name"].getString();
    if (t_node.hasMember("size_bytes"))
        t_db.size_bytes = t_node["size_bytes"].getInt();
    if (t_node.hasMember("source_file") && t_node["source_file"].is_string())
        t_db.source_file = t_node["source_file"].getString();
    if (t_node.hasMember("endianness") && t_node["endianness"].is_string()) {
        const std::string e = t_node["endianness"].getString();
        t_db.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Little;
    }
    if (t_node.hasMember("trigger_events") && t_node["trigger_events"].is_boolean())
        t_db.trigger_events = t_node["trigger_events"].getBool();
    if (t_node.hasMember("modbus_area") && t_node["modbus_area"].is_string()) {
        const std::string a = t_node["modbus_area"].getString();
        if (a == "holding")
            t_db.modbus_area = sgrn::scl::ModbusArea::Holding;
        else if (a == "input")
            t_db.modbus_area = sgrn::scl::ModbusArea::Input;
        else if (a == "coil")
            t_db.modbus_area = sgrn::scl::ModbusArea::Coil;
        else if (a == "discrete")
            t_db.modbus_area = sgrn::scl::ModbusArea::Discrete;
    }
    if (t_node.hasMember("dataset_step_ns") && t_node["dataset_step_ns"].isUint64())
        t_db.dataset_step_ns = t_node["dataset_step_ns"].getUint64();

    if (t_node.hasMember("fields") && t_node["fields"].is_array()) {
        for (const auto& field_node : t_node["fields"].elements()) {
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
    sgrn::json::dom::Document doc;
    doc.parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::db
