#include <sgrn/scl/types/UdtDefinition.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::UdtDefinition& udt_definition) {
    rapidjson::StringBuffer string_buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(string_buffer);
    sgrn::scl::udt::serializeToWriter(writer, udt_definition);
    return string_buffer.GetString();
}

rapidjson::Document toJson(const sgrn::scl::UdtDefinition& udt_definition) {
    rapidjson::Document doc;
    doc.Parse(toJsonString(udt_definition).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::udt
{

sgrn::scl::UdtDefinition fromJson(const rapidjson::Value& t_node) {
    sgrn::scl::UdtDefinition udt_def;
    if (!t_node.IsObject())
        return udt_def;

    if (t_node.HasMember("udt_number"))
        udt_def.udt_number = t_node["udt_number"].GetUint();
    if (t_node.HasMember("name") && t_node["name"].IsString())
        udt_def.name = t_node["name"].GetString();
    if (t_node.HasMember("size_bytes"))
        udt_def.size_bytes = t_node["size_bytes"].GetInt();

    if (t_node.HasMember("fields") && t_node["fields"].IsArray()) {
        for (const auto& field_node : t_node["fields"].GetArray()) {
            udt_def.fields.push_back(sgrn::scl::field::fromJson(field_node));
        }
    }

    if (t_node.HasMember("endianness") && t_node["endianness"].IsString()) {
        const std::string e = t_node["endianness"].GetString();
        udt_def.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.HasMember("trigger_events") && t_node["trigger_events"].IsBool())
        udt_def.trigger_events = t_node["trigger_events"].GetBool();
    if (t_node.HasMember("is_scalar_alias") && t_node["is_scalar_alias"].IsBool())
        udt_def.is_scalar_alias = t_node["is_scalar_alias"].GetBool();
    if (t_node.HasMember("scalar_type") && t_node["scalar_type"].IsString()) {
        sgrn::scl::DataType parsed;
        if (s7codec::stringToType(t_node["scalar_type"].GetString(), parsed))
            udt_def.scalar_type = parsed;
    }
    if (t_node.HasMember("unit") && t_node["unit"].IsString())
        udt_def.unit = t_node["unit"].GetString();
    if (t_node.HasMember("min") && t_node["min"].IsNumber())
        udt_def.min_val = t_node["min"].GetDouble();
    if (t_node.HasMember("max") && t_node["max"].IsNumber())
        udt_def.max_val = t_node["max"].GetDouble();

    if (t_node.HasMember("enum") && t_node["enum"].IsObject()) {
        for (auto it = t_node["enum"].MemberBegin(); it != t_node["enum"].MemberEnd(); ++it) {
            try {
                const int key = std::stoi(it->name.GetString());
                if (it->value.IsString())
                    udt_def.enum_map[key] = it->value.GetString();
            } catch (...) {
            }
        }
    }

    return udt_def;
}

sgrn::scl::UdtDefinition fromJsonString(const std::string& t_value) {
    rapidjson::Document doc;
    doc.Parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::udt
