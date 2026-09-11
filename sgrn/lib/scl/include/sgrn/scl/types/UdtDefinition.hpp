#pragma once

#include <fmt/core.h>
#include <sgrn/scl/types/DataType.hpp>
#include <sgrn/scl/types/DbField.hpp>
#include <map>
#include <optional>
#include <rapidjson/document.h>
#include <s7codec/codec.hpp>
#include <string>
#include <vector>

namespace sgrn::scl
{
struct UdtDefinition {
    uint16_t udt_number{0};
    std::string name;
    int size_bytes{0};
    int max_depth{0};
    std::vector<DbField> fields;
    s7codec::Endian endianness{s7codec::Endian::Big};
    bool trigger_events{false};

    bool is_scalar_alias{false};
    DataType scalar_type{DataType::Byte};
    std::map<int, std::string> enum_map;
    std::optional<std::string> unit;
    std::optional<double> min_val;
    std::optional<double> max_val;
};

} // namespace sgrn::scl

namespace sgrn::scl::udt
{
/// Writer-agnostic JSON "form" for UdtDefinition.
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::UdtDefinition& t_udt) {
    t_writer.StartObject();
    t_writer.Key("udt_number");
    t_writer.Int(t_udt.udt_number);
    t_writer.Key("name");
    t_writer.String(t_udt.name.c_str());
    t_writer.Key("size_bytes");
    t_writer.Int(t_udt.size_bytes);
    t_writer.Key("fields");
    t_writer.StartArray();
    for (const auto& t_field : t_udt.fields) {
        sgrn::scl::field::serializeToWriter(t_writer, t_field);
    }
    t_writer.EndArray();
    if (t_udt.endianness != s7codec::Endian::Big) {
        t_writer.Key("endianness");
        t_writer.String(t_udt.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_udt.trigger_events) {
        t_writer.Key("trigger_events");
        t_writer.Bool(true);
    }
    if (t_udt.is_scalar_alias) {
        t_writer.Key("is_scalar_alias");
        t_writer.Bool(true);
        t_writer.Key("scalar_type");
        t_writer.String(s7codec::s7TypeToString(t_udt.scalar_type));
    }
    if (!t_udt.enum_map.empty()) {
        t_writer.Key("enum");
        t_writer.StartObject();
        for (const auto& [k, v] : t_udt.enum_map) {
            t_writer.Key(std::to_string(k).c_str());
            t_writer.String(v.c_str());
        }
        t_writer.EndObject();
    }
    if (t_udt.unit.has_value()) {
        t_writer.Key("unit");
        t_writer.String(t_udt.unit.value().c_str());
    }
    if (t_udt.min_val.has_value()) {
        t_writer.Key("min");
        t_writer.Double(t_udt.min_val.value());
    }
    if (t_udt.max_val.has_value()) {
        t_writer.Key("max");
        t_writer.Double(t_udt.max_val.value());
    }
    t_writer.EndObject();
}

/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::UdtDefinition fromJson(const rapidjson::Value& t_value);
sgrn::scl::UdtDefinition fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::udt

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::UdtDefinition& t_udt_definition);
rapidjson::Document toJson(const sgrn::scl::UdtDefinition& t_udt_definition);

template <>
struct fmt::formatter<sgrn::scl::UdtDefinition> : formatter<std::string_view> {
    auto format(const sgrn::scl::UdtDefinition& t_udt, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("UdtDefinition{{udt_number={}, name=\"{}\", size_bytes={}, fields={}}}",
                                                       t_udt.udt_number, t_udt.name, t_udt.size_bytes, t_udt.fields.size()),
            t_ctx);
    }
};
