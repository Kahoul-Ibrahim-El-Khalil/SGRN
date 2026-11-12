#pragma once

#include <fmt/format.h>
#include <sgrn/scl/types/DataType.hpp>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <map>
#include <optional>
#include <s7codec/codec.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace sgrn::scl
{
struct UdtDefinition {
    uint16_t udt_number{0};
    std::string name;
    uint32_t size_bytes{0};
    uint8_t max_depth{0};
    std::vector<DbField> fields;
    /// Block-scoped byte order; Big is canonical (see DbField::endianness).
    s7codec::Endian endianness{s7codec::Endian::Little};
    bool trigger_events{false};

    bool is_scalar_alias{false};
    DataType scalar_type{DataType::Byte};
    std::map<int, std::string> enum_map;
    std::optional<std::string> unit;
    std::optional<std::string> dimension;
    std::optional<std::string> description;
    std::optional<int> precision;
    std::optional<double> nominal;
    std::optional<double> alarm_lo;
    std::optional<double> alarm_hi;
    std::optional<double> min_val;
    std::optional<double> max_val;
};

} // namespace sgrn::scl

namespace sgrn::scl::udt
{
/// Writer-agnostic JSON "form" for UdtDefinition.
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::UdtDefinition& t_udt) {
    t_writer.startObject();
    t_writer.key("udt_number");
    t_writer.int32(t_udt.udt_number);
    t_writer.key("name");
    t_writer.string(t_udt.name.c_str());
    t_writer.key("size_bytes");
    t_writer.int32(t_udt.size_bytes);
    t_writer.key("fields");
    t_writer.startArray();
    for (const auto& t_field : t_udt.fields) {
        sgrn::scl::field::serializeToWriter(t_writer, t_field);
    }
    t_writer.endArray();
    if (t_udt.endianness != s7codec::Endian::Little) {
        t_writer.key("endianness");
        t_writer.string(t_udt.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_udt.trigger_events) {
        t_writer.key("trigger_events");
        t_writer.boolean(true);
    }
    if (t_udt.is_scalar_alias) {
        t_writer.key("is_scalar_alias");
        t_writer.boolean(true);
        t_writer.key("scalar_type");
        t_writer.string(s7codec::s7TypeToString(t_udt.scalar_type));
    }
    if (!t_udt.enum_map.empty()) {
        t_writer.key("enum");
        t_writer.startObject();
        for (const auto& [k, v] : t_udt.enum_map) {
            t_writer.key(std::to_string(k).c_str());
            t_writer.string(v.c_str());
        }
        t_writer.endObject();
    }
    if (t_udt.unit.has_value()) {
        t_writer.key("unit");
        t_writer.string(t_udt.unit.value().c_str());
    }
    if (t_udt.dimension.has_value()) {
        t_writer.key("dimension");
        t_writer.string(t_udt.dimension.value().c_str());
    }
    if (t_udt.description.has_value()) {
        t_writer.key("description");
        t_writer.string(t_udt.description.value().c_str());
    }
    if (t_udt.precision.has_value()) {
        t_writer.key("precision");
        t_writer.int32(t_udt.precision.value());
    }
    if (t_udt.nominal.has_value()) {
        t_writer.key("nominal");
        t_writer.float64(t_udt.nominal.value());
    }
    if (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value()) {
        t_writer.key("alarm_lo");
        t_writer.float64(t_udt.alarm_lo.value());
        t_writer.key("alarm_hi");
        t_writer.float64(t_udt.alarm_hi.value());
    }
    if (t_udt.min_val.has_value()) {
        t_writer.key("min");
        t_writer.float64(t_udt.min_val.value());
    }
    if (t_udt.max_val.has_value()) {
        t_writer.key("max");
        t_writer.float64(t_udt.max_val.value());
    }
    t_writer.endObject();
}

/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::UdtDefinition fromJson(const sgrn::json::dom::Value& t_value);
sgrn::scl::UdtDefinition fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::udt

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::UdtDefinition& t_udt_definition);
sgrn::json::dom::Document toJson(const sgrn::scl::UdtDefinition& t_udt_definition);

template <>
struct fmt::formatter<sgrn::scl::UdtDefinition> : formatter<std::string_view> {
    auto format(const sgrn::scl::UdtDefinition& t_udt, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("UdtDefinition{{udt_number={}, name=\"{}\", size_bytes={}, fields={}}}",
                                                       t_udt.udt_number, t_udt.name, t_udt.size_bytes, t_udt.fields.size()),
            t_ctx);
    }
};
