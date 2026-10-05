#pragma once
#include <fmt/core.h>
#include <sgrn/scl/types/DataType.hpp>
#include <map>
#include <optional>
#include <rapidjson/document.h>
#include <s7codec/codec.hpp>
#include <stdexcept>
#include <string>
#include <vector>
namespace sgrn::scl
{
struct DbField {
    std::string name;
    int32_t offset{0};
    uint8_t bit_index{0};
    DataType type{DataType::Byte};
    uint32_t count{0};
    int array_lower_bound{0};
    int array_upper_bound{0};
    uint32_t string_capacity{0};

    std::string udt_name;
    std::vector<DbField> children;
    uint32_t struct_size{0};
    s7codec::Endian endianness{s7codec::Endian::Big};
    std::optional<std::string> unit;
    /// Physical dimension class (e.g. "pressure", "temperature", "flow").
    /// Set by #DIMENSION("..."); constrained by file-level #DIMENSIONS(...).
    /// Describes WHAT is measured; unit describes HOW it is measured.
    std::optional<std::string> dimension;
    /// Human description for dashboards/docs. Set by #DESC("...").
    std::optional<std::string> description;
    /// Display decimals for dashboards. Set by #PRECISION(n).
    std::optional<int> precision;
    /// Expected operating point (Downs & Vogel base case, etc.).
    /// Set by #NOMINAL(v). Intended for residuals, not control.
    std::optional<double> nominal;
    /// Acceptable operating band. Set by #ALARM(lo, hi). Surfaced in
    /// registry/dashboard; evaluation is the consumer's job.
    std::optional<double> alarm_lo;
    std::optional<double> alarm_hi;
    /// Label/metadata marker (#LABEL): sgrn_dataset skips these as features.
    bool is_label{false};
    /// Excluded from WAL persistence and dataset extraction (#TRANSIENT).
    bool is_transient{false};
    /// Write attempts are denied (#READ_ONLY); feeds field-level auth.
    bool is_read_only{false};
    std::optional<double> min_val;
    std::optional<double> max_val;
    std::map<int, std::string> enum_map;
    /// Raw SCL initializer text captured after `:=` (e.g. "THREE", "42", "TRUE").
    /// Seeded into DB memory when the schema is loaded into a PlcMemory.
    std::string init_value;
    bool trigger_events{false};
    bool is_dynamic{false};
};

enum class FieldKind : uint8_t { Scalar, Enum, String, Struct };

inline constexpr FieldKind kind_of(const DbField& f) noexcept {
    if (f.type == DataType::Struct) {
        return FieldKind::Struct;
    }
    if (f.type == DataType::String || f.type == DataType::WString || f.type == DataType::XString || f.type == DataType::XWString) {
        return FieldKind::String;
    }
    if (!f.enum_map.empty()) {
        return FieldKind::Enum;
    }
    return FieldKind::Scalar;
}
inline const char* to_string(FieldKind k) noexcept {
    switch (k) {
        case FieldKind::Scalar:
            return "Scalar";
        case FieldKind::Enum:
            return "Enum";
        case FieldKind::String:
            return "String";
        case FieldKind::Struct:
            return "Struct";
    }
    return "Unknown";
}
} // namespace sgrn::scl

namespace sgrn::scl::field
{
/// Writer-agnostic JSON "form" for DbField. Serializes the full field
/// (including nested children) into any rapidjson Writer.
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::DbField& t_field) {
    t_writer.StartObject();
    t_writer.Key("name");
    t_writer.String(t_field.name.c_str());
    t_writer.Key("offset");
    t_writer.Int(t_field.offset);
    t_writer.Key("bit");
    t_writer.Int(t_field.bit_index);
    t_writer.Key("type");
    t_writer.String(s7codec::s7TypeToString(t_field.type));

    const bool is_string = t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
                           t_field.type == DataType::XWString;
    if (is_string) {
        if (t_field.struct_size > 0) { // array of strings
            t_writer.Key("count");
            t_writer.Int(t_field.count);
            t_writer.Key("capacity");
            t_writer.Int(t_field.struct_size);
        } else { // scalar string
            t_writer.Key("count");
            t_writer.Int(1);
            t_writer.Key("capacity");
            t_writer.Int(t_field.count);
        }
    } else {
        t_writer.Key("count");
        t_writer.Int(t_field.count);
    }
    if (!t_field.udt_name.empty()) {
        t_writer.Key("udt_name");
        t_writer.String(t_field.udt_name.c_str());
    }
    if (!t_field.children.empty()) {
        t_writer.Key("children");
        t_writer.StartArray();
        for (const auto& child : t_field.children) {
            serializeToWriter(t_writer, child);
        }
        t_writer.EndArray();
    }
    if (t_field.struct_size > 0) {
        t_writer.Key("struct_size");
        t_writer.Int(t_field.struct_size);
    }
    if (t_field.unit.has_value()) {
        t_writer.Key("unit");
        t_writer.String(t_field.unit.value().c_str());
    }
    if (t_field.dimension.has_value()) {
        t_writer.Key("dimension");
        t_writer.String(t_field.dimension.value().c_str());
    }
    if (t_field.description.has_value()) {
        t_writer.Key("description");
        t_writer.String(t_field.description.value().c_str());
    }
    if (t_field.precision.has_value()) {
        t_writer.Key("precision");
        t_writer.Int(t_field.precision.value());
    }
    if (t_field.nominal.has_value()) {
        t_writer.Key("nominal");
        t_writer.Double(t_field.nominal.value());
    }
    if (t_field.alarm_lo.has_value() && t_field.alarm_hi.has_value()) {
        t_writer.Key("alarm_lo");
        t_writer.Double(t_field.alarm_lo.value());
        t_writer.Key("alarm_hi");
        t_writer.Double(t_field.alarm_hi.value());
    }
    if (t_field.is_label) {
        t_writer.Key("is_label");
        t_writer.Bool(true);
    }
    if (t_field.is_transient) {
        t_writer.Key("is_transient");
        t_writer.Bool(true);
    }
    if (t_field.is_read_only) {
        t_writer.Key("is_read_only");
        t_writer.Bool(true);
    }
    if (t_field.description.has_value()) {
        t_writer.Key("description");
        t_writer.String(t_field.description.value().c_str());
    }
    if (t_field.precision.has_value()) {
        t_writer.Key("precision");
        t_writer.Int(t_field.precision.value());
    }
    if (t_field.nominal.has_value()) {
        t_writer.Key("nominal");
        t_writer.Double(t_field.nominal.value());
    }
    if (t_field.alarm_lo.has_value() && t_field.alarm_hi.has_value()) {
        t_writer.Key("alarm_lo");
        t_writer.Double(t_field.alarm_lo.value());
        t_writer.Key("alarm_hi");
        t_writer.Double(t_field.alarm_hi.value());
    }
    if (t_field.is_label) {
        t_writer.Key("is_label");
        t_writer.Bool(true);
    }
    if (t_field.is_transient) {
        t_writer.Key("is_transient");
        t_writer.Bool(true);
    }
    if (t_field.is_read_only) {
        t_writer.Key("is_read_only");
        t_writer.Bool(true);
    }
    if (t_field.min_val.has_value()) {
        t_writer.Key("min");
        t_writer.Double(t_field.min_val.value());
    }
    if (t_field.max_val.has_value()) {
        t_writer.Key("max");
        t_writer.Double(t_field.max_val.value());
    }
    if (!t_field.enum_map.empty()) {
        t_writer.Key("enum");
        t_writer.StartObject();
        for (const auto& [k, v] : t_field.enum_map) {
            t_writer.Key(std::to_string(k).c_str());
            t_writer.String(v.c_str());
        }
        t_writer.EndObject();
    }
    if (!t_field.init_value.empty()) {
        t_writer.Key("init");
        t_writer.String(t_field.init_value.c_str());
    }
    if (t_field.endianness != s7codec::Endian::Big) {
        t_writer.Key("endianness");
        t_writer.String(t_field.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_field.trigger_events) {
        t_writer.Key("trigger_events");
        t_writer.Bool(true);
    }
    if (t_field.is_dynamic) {
        t_writer.Key("is_dynamic");
        t_writer.Bool(true);
    }
    t_writer.EndObject();
}

/// from-side lives in its own namespace because C++ cannot overload a global
/// function purely on its return type (DbField/UdtDefinition/DbSchema all
/// parse from the same rapidjson::Value shape).
sgrn::scl::DbField fromJson(const rapidjson::Value& t_value);
sgrn::scl::DbField fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::field

// global namespace — functional JSON form for DbField
std::string toJsonString(const sgrn::scl::DbField& t_field);
rapidjson::Document toJson(const sgrn::scl::DbField& t_field);

template <>
struct fmt::formatter<sgrn::scl::DbField> : formatter<std::string_view> {
    auto format(const sgrn::scl::DbField& t_field, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format(
                "DbField{{name=\"{}\", offset={}, bit_index={}, type={}, count={}, udt=\"{}\", struct_size={}, children={}, unit=\"{}\"}}",
                t_field.name, t_field.offset, t_field.bit_index, t_field.type, t_field.count, t_field.udt_name, t_field.struct_size,
                t_field.children.size(), t_field.unit.value_or("")),
            t_ctx);
    }
};
