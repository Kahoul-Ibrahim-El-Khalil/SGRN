#include <sgrn/scl/types/modbus/ModbusVirtualEntry.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::ModbusVirtualEntry& t_entry) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    sgrn::scl::modbus::entry::serializeToWriter(t_writer, t_entry);
    return sb.GetString();
}

rapidjson::Document toJson(const sgrn::scl::ModbusVirtualEntry& t_entry) {
    rapidjson::Document doc;
    doc.Parse(toJsonString(t_entry).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::modbus::entry
{

sgrn::scl::ModbusVirtualEntry fromJson(const rapidjson::Value& t_node) {
    sgrn::scl::ModbusVirtualEntry t_e;
    if (!t_node.IsObject())
        return t_e;

    if (t_node.HasMember("db_number"))
        t_e.db_number = t_node["db_number"].GetUint();
    if (t_node.HasMember("field_path") && t_node["field_path"].IsString())
        t_e.field_path = t_node["field_path"].GetString();
    if (t_node.HasMember("type") && t_node["type"].IsString()) {
        sgrn::scl::DataType parsed;
        if (s7codec::stringToType(t_node["type"].GetString(), parsed))
            t_e.type = parsed;
    }
    if (t_node.HasMember("byte_offset"))
        t_e.byte_offset = t_node["byte_offset"].GetInt();
    if (t_node.HasMember("bit_index"))
        t_e.bit_index = t_node["bit_index"].GetInt();
    if (t_node.HasMember("byte_count"))
        t_e.byte_count = t_node["byte_count"].GetInt();
    if (t_node.HasMember("reg_start"))
        t_e.reg_start = t_node["reg_start"].GetInt();
    if (t_node.HasMember("reg_count"))
        t_e.reg_count = t_node["reg_count"].GetInt();
    if (t_node.HasMember("padded") && t_node["padded"].IsBool())
        t_e.padded = t_node["padded"].GetBool();
    if (t_node.HasMember("read_only") && t_node["read_only"].IsBool())
        t_e.read_only = t_node["read_only"].GetBool();

    return t_e;
}

sgrn::scl::ModbusVirtualEntry fromJsonString(const std::string& t_value) {
    rapidjson::Document doc;
    doc.Parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::modbus::entry
