#include <sgrn/scl/types/modbus/ModbusVirtualEntry.hpp>
#include <sgrn/utils/json-dom.hpp>

#include <s7codec/codec.hpp>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::ModbusVirtualEntry& t_entry) {
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> t_writer(sb);
    sgrn::scl::modbus::entry::serializeToWriter(t_writer, t_entry);
    return sb.getString();
}

sgrn::json::dom::Document toJson(const sgrn::scl::ModbusVirtualEntry& t_entry) {
    sgrn::json::dom::Document doc;
    doc.parse(toJsonString(t_entry).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::modbus::entry
{

sgrn::scl::ModbusVirtualEntry fromJson(const sgrn::json::dom::Value& t_node) {
    sgrn::scl::ModbusVirtualEntry t_e;
    if (!t_node.is_object())
        return t_e;

    if (t_node.hasMember("number"))
        t_e.db_number = t_node["number"].getUint();
    else if (t_node.hasMember("db_number"))
        t_e.db_number = t_node["db_number"].getUint();
    if (t_node.hasMember("field_path") && t_node["field_path"].is_string())
        t_e.field_path = t_node["field_path"].getString();
    if (t_node.hasMember("type") && t_node["type"].is_string()) {
        if (auto opt = s7codec::toType(t_node["type"].getString()))
            t_e.type = *opt;
    }
    if (t_node.hasMember("byte_offset"))
        t_e.byte_offset = t_node["byte_offset"].getInt();
    if (t_node.hasMember("bit"))
        t_e.bit_index = t_node["bit"].getInt();
    else if (t_node.hasMember("bit_index"))
        t_e.bit_index = t_node["bit_index"].getInt();
    if (t_node.hasMember("byte_count"))
        t_e.byte_count = t_node["byte_count"].getInt();
    if (t_node.hasMember("reg_start"))
        t_e.reg_start = t_node["reg_start"].getInt();
    if (t_node.hasMember("reg_count"))
        t_e.reg_count = t_node["reg_count"].getInt();
    if (t_node.hasMember("padded") && t_node["padded"].is_boolean())
        t_e.padded = t_node["padded"].getBool();
    if (t_node.hasMember("read_only") && t_node["read_only"].is_boolean())
        t_e.read_only = t_node["read_only"].getBool();

    return t_e;
}

sgrn::scl::ModbusVirtualEntry fromJsonString(const std::string& t_value) {
    sgrn::json::dom::Document doc;
    doc.parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::modbus::entry
