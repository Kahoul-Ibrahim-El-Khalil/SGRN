#include <sgrn/scl/types/modbus/ModbusVirtualMap.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::ModbusVirtualMap& t_modbus_virtual_map) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    sgrn::scl::modbus::map::serializeToWriter(t_writer, t_modbus_virtual_map);
    return sb.GetString();
}

rapidjson::Document toJson(const sgrn::scl::ModbusVirtualMap& t_modbus_virtual_map) {
    rapidjson::Document doc;
    doc.Parse(toJsonString(t_modbus_virtual_map).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::modbus::map
{

namespace
{
void loadEntries(const rapidjson::Value& t_node, const char* t_key, std::vector<sgrn::scl::ModbusVirtualEntry>& t_out) {
    if (t_node.HasMember(t_key) && t_node[t_key].IsArray()) {
        for (const auto& e : t_node[t_key].GetArray())
            t_out.push_back(sgrn::scl::modbus::entry::fromJson(e));
    }
}
} // namespace

sgrn::scl::ModbusVirtualMap fromJson(const rapidjson::Value& t_node) {
    sgrn::scl::ModbusVirtualMap t_map;
    if (!t_node.IsObject())
        return t_map;

    loadEntries(t_node, "holding", t_map.holding);
    loadEntries(t_node, "input", t_map.input);
    loadEntries(t_node, "coil", t_map.coil);
    loadEntries(t_node, "discrete", t_map.discrete);

    if (t_node.HasMember("total_holding"))
        t_map.total_holding = t_node["total_holding"].GetInt();
    if (t_node.HasMember("total_input"))
        t_map.total_input = t_node["total_input"].GetInt();
    if (t_node.HasMember("total_coils"))
        t_map.total_coils = t_node["total_coils"].GetInt();
    if (t_node.HasMember("total_discrete"))
        t_map.total_discrete = t_node["total_discrete"].GetInt();

    if (t_node.HasMember("warnings") && t_node["warnings"].IsArray()) {
        for (const auto& warn : t_node["warnings"].GetArray())
            if (warn.IsString())
                t_map.warnings.push_back(warn.GetString());
    }

    return t_map;
}

sgrn::scl::ModbusVirtualMap fromJsonString(const std::string& t_value) {
    rapidjson::Document doc;
    doc.Parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::modbus::map
