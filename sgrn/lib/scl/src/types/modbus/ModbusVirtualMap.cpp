#include <sgrn/scl/types/modbus/ModbusVirtualMap.hpp>
#include <sgrn/utils/json-dom.hpp>

#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::ModbusVirtualMap& t_modbus_virtual_map) {
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> t_writer(sb);
    sgrn::scl::modbus::map::serializeToWriter(t_writer, t_modbus_virtual_map);
    return sb.getString();
}

sgrn::json::dom::Document toJson(const sgrn::scl::ModbusVirtualMap& t_modbus_virtual_map) {
    sgrn::json::dom::Document doc;
    doc.parse(toJsonString(t_modbus_virtual_map).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::modbus::map
{

namespace
{
void loadEntries(const sgrn::json::dom::Value& t_node, const char* t_key, std::vector<sgrn::scl::ModbusVirtualEntry>& t_out) {
    if (t_node.hasMember(t_key) && t_node[t_key].is_array()) {
        for (const auto& e : t_node[t_key].elements())
            t_out.push_back(sgrn::scl::modbus::entry::fromJson(e));
    }
}
} // namespace

sgrn::scl::ModbusVirtualMap fromJson(const sgrn::json::dom::Value& t_node) {
    sgrn::scl::ModbusVirtualMap t_map;
    if (!t_node.is_object())
        return t_map;

    loadEntries(t_node, "holding", t_map.holding);
    loadEntries(t_node, "input", t_map.input);
    loadEntries(t_node, "coil", t_map.coil);
    loadEntries(t_node, "discrete", t_map.discrete);

    if (t_node.hasMember("total_holding"))
        t_map.total_holding = t_node["total_holding"].getInt();
    if (t_node.hasMember("total_input"))
        t_map.total_input = t_node["total_input"].getInt();
    if (t_node.hasMember("total_coils"))
        t_map.total_coils = t_node["total_coils"].getInt();
    if (t_node.hasMember("total_discrete"))
        t_map.total_discrete = t_node["total_discrete"].getInt();

    if (t_node.hasMember("warnings") && t_node["warnings"].is_array()) {
        for (const auto& warn : t_node["warnings"].elements())
            if (warn.is_string())
                t_map.warnings.push_back(warn.getString());
    }

    return t_map;
}

sgrn::scl::ModbusVirtualMap fromJsonString(const std::string& t_value) {
    sgrn::json::dom::Document doc;
    doc.parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::modbus::map
