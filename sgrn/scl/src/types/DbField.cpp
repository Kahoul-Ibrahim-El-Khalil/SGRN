#include <sgrn/scl/types/DbField.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/codec.hpp>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::DbField& t_field) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    sgrn::scl::field::serializeToWriter(t_writer, t_field);
    return sb.GetString();
}

rapidjson::Document toJson(const sgrn::scl::DbField& t_field) {
    rapidjson::Document doc;
    doc.Parse(toJsonString(t_field).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace (C++ cannot overload on return type globally)
// -----------------------------------------------------------------------------

namespace sgrn::scl
{

sgrn::scl::DbField fromJson(const rapidjson::Value& t_node) {
    sgrn::scl::DbField t_field;
    if (!t_node.IsObject())
        return t_field;

    if (t_node.HasMember("name") && t_node["name"].IsString())
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
    if (t_node.HasMember("min") && t_node["min"].IsNumber())
        t_field.min_val = t_node["min"].GetDouble();
    if (t_node.HasMember("max") && t_node["max"].IsNumber())
        t_field.max_val = t_node["max"].GetDouble();

    if (t_node.HasMember("enum") && t_node["enum"].IsObject()) {
        for (auto it = t_node["enum"].MemberBegin(); it != t_node["enum"].MemberEnd(); ++it) {
            try {
                const int key = std::stoi(it->name.GetString());
                if (it->value.IsString())
                    t_field.enum_map[key] = it->value.GetString();
            } catch (...) {
            }
        }
    }
    if (t_node.HasMember("init") && t_node["init"].IsString())
        t_field.init_value = t_node["init"].GetString();
    if (t_node.HasMember("endianness") && t_node["endianness"].IsString()) {
        const std::string e = t_node["endianness"].GetString();
        t_field.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.HasMember("trigger_events") && t_node["trigger_events"].IsBool())
        t_field.trigger_events = t_node["trigger_events"].GetBool();
    if (t_node.HasMember("is_dynamic") && t_node["is_dynamic"].IsBool())
        t_field.is_dynamic = t_node["is_dynamic"].GetBool();

    if (t_node.HasMember("type") && t_node["type"].IsString()) {
        sgrn::scl::DataType parsed;
        if (s7codec::stringToType(t_node["type"].GetString(), parsed)) {
            t_field.type = parsed;
        } else {
            // Unknown type token → a UDT reference (resolved later against the registry).
            t_field.type = sgrn::scl::DataType::Struct;
            t_field.udt_name = t_node["type"].GetString();
        }
    }
    if (t_node.HasMember("children") && t_node["children"].IsArray()) {
        for (const auto& child : t_node["children"].GetArray()) {
            t_field.children.push_back(fromJson(child));
        }
    }

    return t_field;
}

sgrn::scl::DbField fromJsonString(const std::string& t_value) {
    rapidjson::Document doc;
    doc.Parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl
