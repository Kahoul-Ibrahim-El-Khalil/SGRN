#include <fmt/format.h>
#include <sgrn/utils/json-dom.hpp>

#include <sgrn/scl/types/UdtDefinition.hpp>

#include <s7codec/codec.hpp>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::UdtDefinition& udt_definition) {
    sgrn::json::dom::StringBuffer string_buffer;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> writer(string_buffer);
    sgrn::scl::udt::serializeToWriter(writer, udt_definition);
    return string_buffer.getString();
}

sgrn::json::dom::Document toJson(const sgrn::scl::UdtDefinition& udt_definition) {
    sgrn::json::dom::Document doc;
    doc.parse(toJsonString(udt_definition).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::udt
{

sgrn::scl::UdtDefinition fromJson(const sgrn::json::dom::Value& t_node) {
    sgrn::scl::UdtDefinition udt_def;
    if (!t_node.is_object())
        return udt_def;

    if (t_node.hasMember("udt_number"))
        udt_def.udt_number = t_node["udt_number"].getUint();
    if (t_node.hasMember("name") && t_node["name"].is_string())
        udt_def.name = t_node["name"].getString();
    if (t_node.hasMember("size_bytes"))
        udt_def.size_bytes = t_node["size_bytes"].getInt();

    if (t_node.hasMember("fields") && t_node["fields"].is_array()) {
        for (const auto& field_node : t_node["fields"].elements()) {
            udt_def.fields.push_back(sgrn::scl::field::fromJson(field_node));
        }
    }

    if (t_node.hasMember("endianness") && t_node["endianness"].is_string()) {
        const std::string e = t_node["endianness"].getString();
        udt_def.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.hasMember("trigger_events") && t_node["trigger_events"].is_boolean())
        udt_def.trigger_events = t_node["trigger_events"].getBool();
    if (t_node.hasMember("is_scalar_alias") && t_node["is_scalar_alias"].is_boolean())
        udt_def.is_scalar_alias = t_node["is_scalar_alias"].getBool();
    if (t_node.hasMember("scalar_type") && t_node["scalar_type"].is_string()) {
        if (auto opt = s7codec::toType(t_node["scalar_type"].getString()))
            udt_def.scalar_type = *opt;
    }
    if (t_node.hasMember("unit") && t_node["unit"].is_string())
        udt_def.unit = t_node["unit"].getString();
    if (t_node.hasMember("dimension") && t_node["dimension"].is_string())
        udt_def.dimension = t_node["dimension"].getString();
    if (t_node.hasMember("description") && t_node["description"].is_string())
        udt_def.description = t_node["description"].getString();
    if (t_node.hasMember("precision") && t_node["precision"].isInt())
        udt_def.precision = t_node["precision"].getInt();
    if (t_node.hasMember("nominal") && t_node["nominal"].is_number())
        udt_def.nominal = t_node["nominal"].getDouble();
    if (t_node.hasMember("alarm_lo") && t_node["alarm_lo"].is_number())
        udt_def.alarm_lo = t_node["alarm_lo"].getDouble();
    if (t_node.hasMember("alarm_hi") && t_node["alarm_hi"].is_number())
        udt_def.alarm_hi = t_node["alarm_hi"].getDouble();
    if (t_node.hasMember("min") && t_node["min"].is_number())
        udt_def.min_val = t_node["min"].getDouble();
    if (t_node.hasMember("max") && t_node["max"].is_number())
        udt_def.max_val = t_node["max"].getDouble();

    if (t_node.hasMember("enum") && t_node["enum"].is_object()) {
        for (auto it = t_node["enum"].memberBegin(); it != t_node["enum"].memberEnd(); ++it) {
            try {
                const int key = std::stoi(it->name.getString());
                if (it->value.is_string())
                    udt_def.enum_map[key] = it->value.getString();
            } catch (...) {
            }
        }
    }

    return udt_def;
}

sgrn::scl::UdtDefinition fromJsonString(const std::string& t_value) {
    sgrn::json::dom::Document doc;
    doc.parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::udt
