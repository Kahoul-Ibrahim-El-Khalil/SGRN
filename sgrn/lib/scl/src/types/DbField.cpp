#include <fmt/format.h>
#include <sgrn/utils/json-dom.hpp>

#include <sgrn/scl/types/DbField.hpp>

#include <s7codec/codec.hpp>
#include <string>

// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::DbField& t_field) {
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> t_writer(sb);
    sgrn::scl::field::serializeToWriter(t_writer, t_field);
    return sb.getString();
}

sgrn::json::dom::Document toJson(const sgrn::scl::DbField& t_field) {
    sgrn::json::dom::Document doc;
    doc.parse(toJsonString(t_field).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace (C++ cannot overload on return type globally)
// -----------------------------------------------------------------------------

namespace sgrn::scl::field
{

sgrn::scl::DbField fromJson(const sgrn::json::dom::Value& t_node) {
    sgrn::scl::DbField t_field;
    if (!t_node.is_object())
        return t_field;

    if (t_node.hasMember("name") && t_node["name"].is_string())
        t_field.name = t_node["name"].getString();
    if (t_node.hasMember("offset"))
        t_field.offset = t_node["offset"].getInt();
    if (t_node.hasMember("bit_index"))
        t_field.bit_index = t_node["bit_index"].getInt();
    if (t_node.hasMember("count"))
        t_field.count = t_node["count"].getInt();
    if (t_node.hasMember("udt_name") && t_node["udt_name"].is_string())
        t_field.udt_name = t_node["udt_name"].getString();
    if (t_node.hasMember("struct_size"))
        t_field.struct_size = t_node["struct_size"].getInt();
    if (t_node.hasMember("unit") && t_node["unit"].is_string())
        t_field.unit = t_node["unit"].getString();
    if (t_node.hasMember("dimension") && t_node["dimension"].is_string())
        t_field.dimension = t_node["dimension"].getString();
    if (t_node.hasMember("description") && t_node["description"].is_string())
        t_field.description = t_node["description"].getString();
    if (t_node.hasMember("precision") && t_node["precision"].isInt())
        t_field.precision = t_node["precision"].getInt();
    if (t_node.hasMember("nominal") && t_node["nominal"].is_number())
        t_field.nominal = t_node["nominal"].getDouble();
    if (t_node.hasMember("alarm_lo") && t_node["alarm_lo"].is_number())
        t_field.alarm_lo = t_node["alarm_lo"].getDouble();
    if (t_node.hasMember("alarm_hi") && t_node["alarm_hi"].is_number())
        t_field.alarm_hi = t_node["alarm_hi"].getDouble();
    if (t_node.hasMember("is_label") && t_node["is_label"].is_boolean())
        t_field.is_label = t_node["is_label"].getBool();
    if (t_node.hasMember("is_transient") && t_node["is_transient"].is_boolean())
        t_field.is_transient = t_node["is_transient"].getBool();
    if (t_node.hasMember("is_read_only") && t_node["is_read_only"].is_boolean())
        t_field.is_read_only = t_node["is_read_only"].getBool();
    if (t_node.hasMember("min") && t_node["min"].is_number())
        t_field.min_val = t_node["min"].getDouble();
    if (t_node.hasMember("max") && t_node["max"].is_number())
        t_field.max_val = t_node["max"].getDouble();

    if (t_node.hasMember("enum") && t_node["enum"].is_object()) {
        for (auto it = t_node["enum"].memberBegin(); it != t_node["enum"].memberEnd(); ++it) {
            try {
                const int key = std::stoi(it->name.getString());
                if (it->value.is_string())
                    t_field.enum_map[key] = it->value.getString();
            } catch (...) {
            }
        }
    }
    if (t_node.hasMember("init") && t_node["init"].is_string())
        t_field.init_value = t_node["init"].getString();
    if (t_node.hasMember("endianness") && t_node["endianness"].is_string()) {
        const std::string e = t_node["endianness"].getString();
        t_field.endianness = (e == "little" || e == "LITTLE") ? s7codec::Endian::Little : s7codec::Endian::Big;
    }
    if (t_node.hasMember("trigger_events") && t_node["trigger_events"].is_boolean())
        t_field.trigger_events = t_node["trigger_events"].getBool();
    if (t_node.hasMember("is_dynamic") && t_node["is_dynamic"].is_boolean())
        t_field.is_dynamic = t_node["is_dynamic"].getBool();

    if (t_node.hasMember("agg") && t_node["agg"].is_string()) {
        const std::string a = t_node["agg"].getString();
        if (a == "MEAN" || a == "AVG")
            t_field.agg_type = AggregationType::Mean;
        else if (a == "MAX")
            t_field.agg_type = AggregationType::Max;
        else if (a == "MIN")
            t_field.agg_type = AggregationType::Min;
        else if (a == "OR")
            t_field.agg_type = AggregationType::Or;
        else if (a == "LAST")
            t_field.agg_type = AggregationType::Last;
    }
    if (t_node.hasMember("is_timestamp") && t_node["is_timestamp"].is_boolean())
        t_field.is_timestamp = t_node["is_timestamp"].getBool();
    if (t_node.hasMember("timestamp_for") && t_node["timestamp_for"].is_array()) {
        for (const auto& item : t_node["timestamp_for"].elements()) {
            if (item.is_string())
                t_field.timestamp_for.emplace_back(item.getString());
        }
    }
    if (t_node.hasMember("dataset_step_ns") && t_node["dataset_step_ns"].isUint64())
        t_field.dataset_step_ns = t_node["dataset_step_ns"].getUint64();

    if (t_node.hasMember("type") && t_node["type"].is_string()) {
        if (auto opt = s7codec::toType(t_node["type"].getString())) {
            t_field.type = *opt;
        } else {
            // Unknown type token → a UDT reference (resolved later against the registry).
            t_field.type = sgrn::scl::DataType::Struct;
            t_field.udt_name = t_node["type"].getString();
        }
    }
    if (t_node.hasMember("children") && t_node["children"].is_array()) {
        for (const auto& child : t_node["children"].elements()) {
            t_field.children.push_back(fromJson(child));
        }
    }

    return t_field;
}

sgrn::scl::DbField fromJsonString(const std::string& t_value) {
    sgrn::json::dom::Document doc;
    doc.parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::field
