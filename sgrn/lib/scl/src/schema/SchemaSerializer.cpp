#include <fmt/core.h>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/utils.hpp>
#include <sgrn/utils/glaze.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::scl
{

namespace detail
{

static int fieldSpanBytes(const DbField& t_field) {
    if (t_field.type == DataType::Struct)
        return std::max(1, static_cast<int>(t_field.struct_size)) * std::max(1, static_cast<int>(t_field.count));
    // For strings, struct_size holds the per-element byte span (after offset-tracker fix).
    if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
        t_field.type == DataType::XWString) {
        const int elem_span = t_field.struct_size > 0 ? static_cast<int>(t_field.struct_size) : [&]() -> int {
            auto opt = s7codec::typeSpanBytes(t_field.type, t_field.string_capacity > 0 ? t_field.string_capacity : t_field.count);
            return opt ? static_cast<int>(*opt) : 0;
        }();
        return elem_span * std::max(1, static_cast<int>(t_field.count));
    }
    auto opt = s7codec::typeSpanBytes(t_field.type, t_field.count);
    return opt ? static_cast<int>(*opt) : 0;
}

static void resolveUdtInField(DbField& t_field, const PlcSchemaStore& t_registry) {
    if (t_field.type == DataType::Struct && !t_field.udt_name.empty() && t_field.children.empty()) {
        auto udt_res = t_registry.getUdtByName(t_field.udt_name);
        if (udt_res.has_value()) {
            const UdtDefinition* p_udt = udt_res.value();
            t_field.children = p_udt->fields;
            t_field.struct_size = p_udt->size_bytes;
            // Late-attached children keep the UDT's scope — restamp to the
            // instantiating block's scope (no field-level endianness).
            stampDescendantEndianness(t_field);
        }
    }
    for (auto& child : t_field.children) {
        resolveUdtInField(child, t_registry);
    }
}

static int extractTrailingNumber(const std::string& t_value) {
    std::smatch m;
    static const std::regex kSuffixRe(R"((\d+)\s*$)");
    if (std::regex_search(t_value, m, kSuffixRe))
        return sgrn::utils::strings::parseInt(m[1].str()).value_or(0);
    return 0;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// JSON mirror structs
//
// The schema JSON layout is a compatibility contract (files on disk, JSONL
// WALs). Rather than hand-writing JSON key gymnastics, each on-disk shape
// is mirrored by a Json-suffixed struct (see MbProxyConfigJson) whose member
// names match the JSON keys, so plain Glaze reflection reads and writes it.
// sgrn::json::writeOmitNull skips
// unset optionals, which reproduces the historical rule "only emit non-
// defaults, never emit nulls"; on read, unset optionals leave the domain
// struct's defaults in place.
//
// Members that exist only as read aliases of a canonical key
// ("bit_index", "db_number", "db_name") are never set on write.
//
// glz::meta is needed only for FieldJson/UdtJson because the "enum" key is not
// a valid C++ identifier.
// ─────────────────────────────────────────────────────────────────────────────

namespace detail
{

struct FieldJson {
    std::optional<std::string> name;
    int offset{0};
    int bit{0};
    std::optional<int> bit_index; ///< legacy alias of "bit"
    std::optional<std::string> type;
    uint32_t count{0};
    std::optional<uint32_t> capacity; ///< string char capacity (string types only)
    std::optional<std::string> udt_name;
    std::optional<std::vector<FieldJson>> children;
    std::optional<uint32_t> struct_size;
    std::optional<std::string> unit;
    std::optional<std::string> dimension;
    std::optional<std::string> description;
    std::optional<int> precision;
    std::optional<double> nominal;
    std::optional<double> alarm_lo;
    std::optional<double> alarm_hi;
    std::optional<bool> is_label;
    std::optional<bool> is_transient;
    std::optional<bool> is_read_only;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<std::map<std::string, std::string>> enum_entries; ///< "enum": stringified int keys
    std::optional<std::string> endianness;
    std::optional<bool> trigger_events;
    std::optional<bool> is_dynamic;
};

struct UdtJson {
    int udt_number{0};
    std::string name;
    int size_bytes{0};
    std::vector<FieldJson> fields;
    std::optional<std::string> endianness;
    std::optional<bool> trigger_events;
    std::optional<bool> is_scalar_alias;
    std::optional<std::string> scalar_type;
    std::optional<std::map<std::string, std::string>> enum_entries;
    std::optional<std::string> unit;
    std::optional<std::string> dimension;
    std::optional<std::string> description;
    std::optional<int> precision;
    std::optional<double> nominal;
    std::optional<double> alarm_lo;
    std::optional<double> alarm_hi;
    std::optional<double> min;
    std::optional<double> max;
};

struct DbJson {
    std::optional<int> number;
    std::optional<int> db_number; ///< legacy alias of "number"
    std::optional<std::string> name;
    std::optional<std::string> db_name; ///< legacy alias of "name"
    int size_bytes{0};
    std::optional<std::string> source_file;
    std::optional<std::string> endianness;
    std::optional<bool> trigger_events;
    std::optional<std::string> modbus_area;
    std::optional<std::vector<FieldJson>> fields; ///< unset = headers-only form
};

struct TagJson {
    std::optional<std::string> name;
    std::string table;
    std::string type;
    std::optional<std::string> remark;
    std::optional<std::string> address;
};

struct SummaryJson {
    int total_dbs{0};
    int total_tags{0};
    int accessible_dbs{0};
    int total_udts{0};
    int warnings{0};
    std::optional<int> filtered_db;
};

struct RootJson {
    std::vector<DbJson> dbs;
    std::optional<std::vector<UdtJson>> udts; ///< only present when not DB-filtered
    std::optional<std::vector<TagJson>> tags;
    std::optional<std::vector<std::string>> dimensions;
    std::optional<std::vector<std::string>> warnings; ///< read side only (parseContent)
    SummaryJson summary;
};

} // namespace detail

} // namespace sgrn::scl

// Full meta tables are required only for FieldJson/UdtJson: the "enum" JSON
// key is not a valid C++ identifier, so plain member-name reflection cannot
// cover those two wire structs. Everything else is pure reflection.
namespace glz
{
template <>
struct meta<sgrn::scl::detail::FieldJson> {
    using T = sgrn::scl::detail::FieldJson;
    static constexpr auto value = object("name", &T::name, "offset", &T::offset, "bit", &T::bit, "bit_index", &T::bit_index, "type",
        &T::type, "count", &T::count, "capacity", &T::capacity, "udt_name", &T::udt_name, "children", &T::children, "struct_size",
        &T::struct_size, "unit", &T::unit, "dimension", &T::dimension, "description", &T::description, "precision", &T::precision,
        "nominal", &T::nominal, "alarm_lo", &T::alarm_lo, "alarm_hi", &T::alarm_hi, "is_label", &T::is_label, "is_transient",
        &T::is_transient, "is_read_only", &T::is_read_only, "min", &T::min, "max", &T::max, "enum", &T::enum_entries, "endianness",
        &T::endianness, "trigger_events", &T::trigger_events, "is_dynamic", &T::is_dynamic);
};

template <>
struct meta<sgrn::scl::detail::UdtJson> {
    using T = sgrn::scl::detail::UdtJson;
    static constexpr auto value =
        object("udt_number", &T::udt_number, "name", &T::name, "size_bytes", &T::size_bytes, "fields", &T::fields, "endianness",
            &T::endianness, "trigger_events", &T::trigger_events, "is_scalar_alias", &T::is_scalar_alias, "scalar_type", &T::scalar_type,
            "enum", &T::enum_entries, "unit", &T::unit, "dimension", &T::dimension, "description", &T::description, "precision",
            &T::precision, "nominal", &T::nominal, "alarm_lo", &T::alarm_lo, "alarm_hi", &T::alarm_hi, "min", &T::min, "max", &T::max);
};
} // namespace glz

namespace sgrn::scl
{

namespace detail
{

static bool isStringType(DataType t_type) {
    return t_type == DataType::String || t_type == DataType::WString || t_type == DataType::XString || t_type == DataType::XWString;
}

static std::map<std::string, std::string> enumToJson(const std::map<int, std::string>& t_enum_map) {
    std::map<std::string, std::string> out;
    for (const auto& [key, value] : t_enum_map)
        out.emplace(std::to_string(key), value);
    return out;
}

// ── domain → wire ─────────────────────────────────────────────────────────────

static FieldJson fieldToJson(const DbField& t_f) {
    FieldJson w;
    w.name = t_f.name;
    w.offset = t_f.offset;
    w.bit = t_f.bit_index;
    w.type = s7codec::s7TypeToString(t_f.type);
    w.count = t_f.count;
    if (isStringType(t_f.type)) {
        // After the offset-tracker fix:
        //   scalar string:  count=1,  string_capacity=chars, struct_size=byte_span
        //   string array:   count=N,  string_capacity=chars, struct_size=per-elem-byte_span
        // Prefer string_capacity; fall back to deriving chars from the byte span,
        // then to the legacy count fallback.
        if (t_f.string_capacity > 0) {
            w.capacity = t_f.string_capacity;
        } else if (t_f.struct_size > 0) {
            const int hdr = (t_f.type == DataType::String) ? 2 : (t_f.type == DataType::WString) ? 4 : 8; // XString / XWString
            const int wscale = (t_f.type == DataType::WString || t_f.type == DataType::XWString) ? 2 : 1;
            w.capacity = static_cast<uint32_t>((t_f.struct_size - hdr) / wscale);
        } else {
            w.capacity = t_f.count;
        }
    }
    if (!t_f.udt_name.empty())
        w.udt_name = t_f.udt_name;
    if (!t_f.children.empty()) {
        std::vector<FieldJson> children;
        children.reserve(t_f.children.size());
        for (const auto& child : t_f.children)
            children.push_back(fieldToJson(child));
        w.children = std::move(children);
    }
    if (t_f.struct_size > 0)
        w.struct_size = t_f.struct_size;
    w.unit = t_f.unit;
    w.dimension = t_f.dimension;
    w.description = t_f.description;
    w.precision = t_f.precision;
    w.nominal = t_f.nominal;
    if (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value()) {
        w.alarm_lo = t_f.alarm_lo;
        w.alarm_hi = t_f.alarm_hi;
    }
    if (t_f.is_label)
        w.is_label = true;
    if (t_f.is_transient)
        w.is_transient = true;
    if (t_f.is_read_only)
        w.is_read_only = true;
    w.min = t_f.min_val;
    w.max = t_f.max_val;
    if (!t_f.enum_map.empty())
        w.enum_entries = enumToJson(t_f.enum_map);
    if (t_f.endianness != s7codec::Endian::Little)
        w.endianness = "big"; // defensive: memory is Little-only, but round-trip old values
    if (t_f.trigger_events)
        w.trigger_events = true;
    if (t_f.is_dynamic)
        w.is_dynamic = true;
    return w;
}

static UdtJson udtToJson(const UdtDefinition& t_u) {
    UdtJson w;
    w.udt_number = t_u.udt_number;
    w.name = t_u.name;
    w.size_bytes = t_u.size_bytes;
    w.fields.reserve(t_u.fields.size());
    for (const auto& f : t_u.fields)
        w.fields.push_back(fieldToJson(f));
    if (t_u.endianness != s7codec::Endian::Little)
        w.endianness = "big";
    if (t_u.trigger_events)
        w.trigger_events = true;
    if (t_u.is_scalar_alias) {
        w.is_scalar_alias = true;
        w.scalar_type = s7codec::s7TypeToString(t_u.scalar_type);
    }
    if (!t_u.enum_map.empty())
        w.enum_entries = enumToJson(t_u.enum_map);
    w.unit = t_u.unit;
    w.dimension = t_u.dimension;
    w.description = t_u.description;
    w.precision = t_u.precision;
    w.nominal = t_u.nominal;
    if (t_u.alarm_lo.has_value() && t_u.alarm_hi.has_value()) {
        w.alarm_lo = t_u.alarm_lo;
        w.alarm_hi = t_u.alarm_hi;
    }
    w.min = t_u.min_val;
    w.max = t_u.max_val;
    return w;
}

static DbJson dbToJson(const DbSchema& t_db, bool t_headers_only) {
    DbJson w;
    w.number = t_db.db_number;
    w.name = t_db.db_name;
    w.size_bytes = t_db.size_bytes;
    if (!t_db.source_file.empty())
        w.source_file = t_db.source_file;
    if (t_db.endianness != s7codec::Endian::Little)
        w.endianness = "big";
    if (t_db.trigger_events)
        w.trigger_events = true;
    if (t_db.modbus_area != ModbusArea::None) {
        switch (t_db.modbus_area) {
            case ModbusArea::Holding:
                w.modbus_area = "holding";
                break;
            case ModbusArea::Input:
                w.modbus_area = "input";
                break;
            case ModbusArea::Coil:
                w.modbus_area = "coil";
                break;
            case ModbusArea::Discrete:
                w.modbus_area = "discrete";
                break;
            default:
                break;
        }
    }
    if (!t_headers_only) {
        std::vector<FieldJson> fields;
        fields.reserve(t_db.fields.size());
        for (const auto& f : t_db.fields)
            fields.push_back(fieldToJson(f));
        w.fields = std::move(fields);
    }
    return w;
}

static TagJson tagToJson(const PlcTag& t_tag) {
    TagJson w;
    w.name = t_tag.name;
    w.table = t_tag.table_name;
    w.type = t_tag.type_str;
    if (!t_tag.remark.empty())
        w.remark = t_tag.remark;
    w.address = t_tag.addr.label;
    return w;
}

static RootJson rootToJson(const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number, bool t_headers_only) {
    RootJson root;
    int accessible_dbs = 0;
    for (const auto& [num, db] : t_registry.dbs()) {
        if (t_db_number.has_value() && num != *t_db_number)
            continue;
        root.dbs.push_back(dbToJson(db, t_headers_only));
        ++accessible_dbs;
    }
    if (!t_db_number.has_value()) {
        std::vector<UdtJson> udts;
        udts.reserve(t_registry.udts().size());
        for (const UdtDefinition& udt : t_registry.udts())
            udts.push_back(udtToJson(udt));
        root.udts = std::move(udts);

        std::vector<TagJson> tags;
        tags.reserve(t_registry.tags().size());
        for (const auto& [name, tag] : t_registry.tags())
            tags.push_back(tagToJson(tag));
        root.tags = std::move(tags);

        if (!t_registry.dimensions().empty())
            root.dimensions = {t_registry.dimensions().begin(), t_registry.dimensions().end()};
    }
    root.summary.total_dbs = static_cast<int>(t_registry.dbs().size());
    root.summary.total_tags = static_cast<int>(t_registry.tags().size());
    root.summary.accessible_dbs = accessible_dbs;
    root.summary.total_udts = static_cast<int>(t_registry.udts().size());
    root.summary.warnings = static_cast<int>(t_registry.warnings().size());
    if (t_db_number.has_value())
        root.summary.filtered_db = static_cast<int>(*t_db_number);
    return root;
}

// ── wire → domain ─────────────────────────────────────────────────────────────

static sgrn::Result<DbField, SclError> fieldFromJson(const FieldJson& t_w) {
    if (!t_w.name.has_value() || !t_w.type.has_value())
        return SclError::Generic;

    DbField f;
    f.name = *t_w.name;
    f.offset = t_w.offset;
    f.bit_index = static_cast<uint8_t>(t_w.bit != 0 ? t_w.bit : t_w.bit_index.value_or(0));
    f.count = t_w.count;
    if (t_w.udt_name)
        f.udt_name = *t_w.udt_name;
    if (t_w.struct_size)
        f.struct_size = *t_w.struct_size;
    f.unit = t_w.unit;
    f.dimension = t_w.dimension;
    f.description = t_w.description;
    f.precision = t_w.precision;
    f.nominal = t_w.nominal;
    f.alarm_lo = t_w.alarm_lo;
    f.alarm_hi = t_w.alarm_hi;
    if (t_w.is_label)
        f.is_label = *t_w.is_label;
    if (t_w.is_transient)
        f.is_transient = *t_w.is_transient;
    if (t_w.is_read_only)
        f.is_read_only = *t_w.is_read_only;
    if (t_w.min)
        f.min_val = *t_w.min;
    if (t_w.max)
        f.max_val = *t_w.max;
    if (t_w.enum_entries) {
        for (const auto& [key, value] : *t_w.enum_entries) {
            try {
                f.enum_map[std::stoi(key)] = value;
            } catch (...) {
                // Non-numeric enum keys are ignored, as before.
            }
        }
    }
    // Memory endianness is Little-only now (see DbField.hpp); the "endianness"
    // key is accepted and ignored so old files keep loading.
    if (t_w.trigger_events)
        f.trigger_events = *t_w.trigger_events;
    if (t_w.is_dynamic)
        f.is_dynamic = *t_w.is_dynamic;

    // "type": a known S7 type name, or an implicit UDT reference.
    if (auto type = parseDataType(*t_w.type)) {
        f.type = *type;
    } else {
        f.type = DataType::Struct;
        f.udt_name = *t_w.type;
    }

    // String fields carry 'capacity' (chars); restore string_capacity and
    // recompute the per-element byte span so the field matches what the SCL
    // parser would produce.
    if (isStringType(f.type) && t_w.capacity.has_value()) {
        const uint32_t char_cap = *t_w.capacity;
        f.string_capacity = char_cap;
        if (f.count < 1)
            f.count = 1;
        const auto span = s7codec::typeSpanBytes(f.type, char_cap);
        f.struct_size = span ? static_cast<uint32_t>(*span) : 0;
    }

    if (t_w.children) {
        for (const auto& child : *t_w.children) {
            auto child_field = fieldFromJson(child);
            if (child_field.hasError())
                return Error(child_field.error());
            f.children.push_back(std::move(child_field.value()));
        }
    }
    return f;
}

static sgrn::Result<UdtDefinition, SclError> udtFromJson(const UdtJson& t_w) {
    UdtDefinition u;
    u.udt_number = static_cast<uint16_t>(t_w.udt_number);
    u.name = t_w.name;
    u.size_bytes = t_w.size_bytes;
    for (const auto& fw : t_w.fields) {
        auto f = fieldFromJson(fw);
        if (f.hasError())
            return Error(f.error());
        u.fields.push_back(std::move(f.value()));
    }
    // Memory endianness is Little-only now; the "endianness" key is ignored.
    if (t_w.trigger_events)
        u.trigger_events = *t_w.trigger_events;
    if (t_w.is_scalar_alias)
        u.is_scalar_alias = *t_w.is_scalar_alias;
    if (t_w.scalar_type) {
        if (auto t = s7codec::toType(t_w.scalar_type->c_str()))
            u.scalar_type = *t;
    }
    if (t_w.enum_entries) {
        for (const auto& [key, value] : *t_w.enum_entries) {
            try {
                u.enum_map[std::stoi(key)] = value;
            } catch (...) {
            }
        }
    }
    u.unit = t_w.unit;
    u.dimension = t_w.dimension;
    u.description = t_w.description;
    u.precision = t_w.precision;
    u.nominal = t_w.nominal;
    u.alarm_lo = t_w.alarm_lo;
    u.alarm_hi = t_w.alarm_hi;
    if (t_w.min)
        u.min_val = *t_w.min;
    if (t_w.max)
        u.max_val = *t_w.max;
    return u;
}

static sgrn::Result<DbSchema, SclError> dbFromJson(const DbJson& t_w) {
    DbSchema db;
    if (t_w.number)
        db.db_number = static_cast<uint16_t>(*t_w.number);
    else if (t_w.db_number)
        db.db_number = static_cast<uint16_t>(*t_w.db_number);
    if (t_w.name)
        db.db_name = *t_w.name;
    else if (t_w.db_name)
        db.db_name = *t_w.db_name;
    db.size_bytes = t_w.size_bytes;
    if (t_w.source_file)
        db.source_file = *t_w.source_file;
    // Memory endianness is Little-only now; the "endianness" key is ignored.
    if (t_w.trigger_events)
        db.trigger_events = *t_w.trigger_events;
    if (t_w.modbus_area) {
        const std::string& area = *t_w.modbus_area;
        if (area == "holding")
            db.modbus_area = ModbusArea::Holding;
        else if (area == "input")
            db.modbus_area = ModbusArea::Input;
        else if (area == "coil")
            db.modbus_area = ModbusArea::Coil;
        else if (area == "discrete")
            db.modbus_area = ModbusArea::Discrete;
    }
    for (const auto& fw : t_w.fields.value_or({})) {
        auto f = fieldFromJson(fw);
        if (f.hasError())
            return Error(f.error());
        db.fields.push_back(std::move(f.value()));
    }
    if (db.size_bytes == 0) {
        int max_end = 0;
        for (const DbField& f : db.fields)
            max_end = std::max(max_end, f.offset + fieldSpanBytes(f));
        db.size_bytes = max_end;
    }
    return db;
}

static sgrn::Result<PlcTag, SclError> tagFromJson(const TagJson& t_w) {
    if (!t_w.name.has_value())
        return SclError::Generic;
    PlcTag tag;
    tag.name = *t_w.name;
    tag.table_name = t_w.table;
    tag.type_str = t_w.type;
    // Resolve scalar types now; UDT references (quoted or bare names) resolve
    // against the schema at load time (see PlcRuntime tags).
    if (auto t = parseS7Type(tag.type_str))
        tag.type = *t;
    else if (!tag.type_str.empty())
        tag.udt_name = tag.type_str;
    if (t_w.remark)
        tag.remark = *t_w.remark;
    if (t_w.address) {
        if (auto addr = parsePlcAddress(*t_w.address))
            tag.addr = *addr;
        else
            return SclError::Generic;
    }
    return tag;
}

static sgrn::Result<RegistryContent, SclError> contentFromJson(const RootJson& t_w, bool t_lenient_tags) {
    RegistryContent out;
    for (const auto& uw : t_w.udts.value_or({})) {
        auto udt = udtFromJson(uw);
        if (udt.hasError())
            return Error(udt.error());
        out.udts.push_back(std::move(udt.value()));
    }
    for (const auto& dw : t_w.dbs) {
        auto db = dbFromJson(dw);
        if (db.hasError())
            return Error(db.error());
        if (db.value().db_number <= 0 && !db.value().db_name.empty())
            db.value().db_number = static_cast<uint16_t>(extractTrailingNumber(db.value().db_name));
        if (db.value().db_number <= 0)
            return SclError::Generic;
        out.dbs.push_back(std::move(db.value()));
    }
    for (const auto& tw : t_w.tags.value_or({})) {
        auto tag = tagFromJson(tw);
        if (tag.hasError()) {
            if (t_lenient_tags)
                continue;
            return Error(tag.error());
        }
        out.tags.push_back(std::move(tag.value()));
    }
    if (t_w.dimensions)
        out.dimensions = *t_w.dimensions;
    if (t_w.warnings)
        out.warnings = *t_w.warnings;
    return out;
}

static sgrn::Result<RegistryContent, SclError> parseContent(std::string_view t_text, bool t_lenient_tags) {
    // Legacy form: a bare array of DB objects.
    const size_t first = t_text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && t_text[first] == '[') {
        auto dbs = sgrn::json::read<std::vector<DbJson>>(t_text);
        if (dbs.hasError())
            return SclError::ParseError;
        RootJson root;
        root.dbs = std::move(dbs.value());
        return contentFromJson(root, t_lenient_tags);
    }

    auto root = sgrn::json::read<RootJson>(t_text);
    if (root.hasError())
        return SclError::ParseError;
    return contentFromJson(root.value(), t_lenient_tags);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// SchemaSerializer — JSON
// ─────────────────────────────────────────────────────────────────────────────

std::string SchemaSerializer::udtToJson(const UdtDefinition& t_udt) {
    return sgrn::json::writeOmitNull(detail::udtToJson(t_udt)).value_or("");
}

std::string SchemaSerializer::dbToJson(const DbSchema& t_db) {
    return sgrn::json::writeOmitNull(detail::dbToJson(t_db, false)).value_or("");
}

std::string SchemaSerializer::tagToJson(const PlcTag& t_tag) {
    return sgrn::json::writeOmitNull(detail::tagToJson(t_tag)).value_or("");
}

std::string SchemaSerializer::serialize(
    const PlcSchemaStore& t_registry, std::optional<uint16_t> t_db_number, bool t_headers_only, bool t_pretty) {
    const auto wire = detail::rootToJson(t_registry, t_db_number, t_headers_only);
    const auto out = t_pretty ? sgrn::json::writeOmitNullPretty(wire) : sgrn::json::writeOmitNull(wire);
    // Plain mirror structs cannot fail to write; degrade to an empty payload
    // rather than changing the std::string public API.
    return out.value_or("");
}

sgrn::Result<UdtDefinition, scl::SclError> SchemaSerializer::udtFromJson(std::string_view t_text) {
    auto wire = sgrn::json::read<detail::UdtJson>(t_text);
    if (wire.hasError())
        return SclError::ParseError;
    return detail::udtFromJson(wire.value());
}

sgrn::Result<DbSchema, scl::SclError> SchemaSerializer::dbFromJson(std::string_view t_text) {
    auto wire = sgrn::json::read<detail::DbJson>(t_text);
    if (wire.hasError())
        return SclError::ParseError;
    return detail::dbFromJson(wire.value());
}

sgrn::Result<PlcTag, scl::SclError> SchemaSerializer::tagFromJson(std::string_view t_text) {
    auto wire = sgrn::json::read<detail::TagJson>(t_text);
    if (wire.hasError())
        return SclError::ParseError;
    return detail::tagFromJson(wire.value());
}

sgrn::Result<RegistryContent, scl::SclError> SchemaSerializer::parseContent(std::string_view t_text, bool t_lenient_tags) {
    return detail::parseContent(t_text, t_lenient_tags);
}

sgrn::Result<void, scl::SclError> SchemaSerializer::deserialize(PlcSchemaStore& t_registry, std::string_view t_text) {
    auto content = parseContent(t_text);
    if (content.hasError())
        return Error(content.error());

    for (auto& udt : content.value().udts) {
        if (auto res = t_registry.addUdt(std::move(udt)); res.hasError())
            return Error(res.error());
    }
    for (auto& db : content.value().dbs)
        t_registry.dbs_[db.db_number] = std::move(db);
    // Duplicate tags (Conflict) are skipped silently, as before.
    for (auto& tag : content.value().tags)
        (void)t_registry.addTag(std::move(tag));

    t_registry.rebuildIndices();
    resolveUdtsInRegistry(t_registry);
    t_registry.rebuildIndices();
    return {};
}

void SchemaSerializer::resolveUdtsInRegistry(PlcSchemaStore& t_registry) {
    for (auto& [_, t_db] : t_registry.dbs_) {
        for (auto& t_field : t_db.fields) {
            detail::resolveUdtInField(t_field, t_registry);
        }
    }
    for (auto& p_udt : t_registry.udts_) {
        for (auto& t_field : p_udt.fields) {
            detail::resolveUdtInField(t_field, t_registry);
        }
    }
}

} // namespace sgrn::scl

// ─────────────────────────────────────────────────────────────────────────────
// Binary schema codec (archive header, WAL version 4+)
//
// Layout (all integers little-endian; strings are u32 length + UTF-8 bytes):
//   "SGRS" magic + codec_ver:u16 + flags:u16(reserved)
//   db_count:u32, udt_count:u32, tag_count:u32, then the three sections.
// Field record: name, offset:i32, bit:u8, type name, count:u32,
//   array bounds:i32 x2, string_capacity:u32, struct_size:u32, flags:u16,
//   then optionals in flag order (udt_name, children, unit, min:f64,
//   max:f64, enum_map, init_value, dimension), then, when bit 15 is set,
//   an extended block (ext:u16, then desc, precision:u8, nominal:f64,
//   alarm_lo:f64 + alarm_hi:f64 in ext-bit order). Flags also carry
//   trigger_events, is_dynamic, and the 2-bit endianness (0=Big, 1=Little,
//   2=Unknown). NOTE: records WITH dimension or extended metadata require
//   a reader that knows the new bits; old readers only stay compatible
//   with schemas that don't use them.
// Unlike the JSON form this is lossless: array bounds, init values, UDT
// alias detail, and full tag addresses all survive the round-trip.
// ─────────────────────────────────────────────────────────────────────────────

namespace sgrn::scl
{
namespace bin
{

// Field presence/property flags (u16).
inline constexpr uint16_t kHasUdt = 1 << 0;
inline constexpr uint16_t kHasChildren = 1 << 1;
inline constexpr uint16_t kHasUnit = 1 << 2;
inline constexpr uint16_t kHasMin = 1 << 3;
inline constexpr uint16_t kHasMax = 1 << 4;
inline constexpr uint16_t kHasEnum = 1 << 5;
inline constexpr uint16_t kHasInit = 1 << 6;
inline constexpr uint16_t kTriggerEvents = 1 << 7;
inline constexpr uint16_t kIsDynamic = 1 << 8;
inline constexpr uint16_t kHasDimension = 1 << 9;
// Bit 15: extended display metadata block (u16 ext flags + optionals).
// Keeps the base record shape stable; readers that predate the block must
// refuse records with this bit (same one-way note as dimension).
inline constexpr uint16_t kHasExt = 1 << 15;
// Extended block bits (u16, written only when kHasExt is set).
inline constexpr uint16_t kExtHasDesc = 1 << 0;
inline constexpr uint16_t kExtHasPrecision = 1 << 1;
inline constexpr uint16_t kExtHasNominal = 1 << 2;
inline constexpr uint16_t kExtHasAlarm = 1 << 3;
inline constexpr uint16_t kExtIsLabel = 1 << 4;
inline constexpr uint16_t kExtIsTransient = 1 << 5;
inline constexpr uint16_t kExtIsReadOnly = 1 << 6;
// Bits 10-11: endianness (0 = Big default, 1 = Little, 2 = Unknown).
inline constexpr uint16_t kEndianShift = 10;
inline constexpr uint16_t kEndianMask = 0x3 << kEndianShift;

// DB flags (u8): bit0 = source_file present.
inline constexpr uint8_t kDbHasSourceFile = 1 << 0;
// UDT flags (u8): bit0 = enum_map, bit1 = unit, bit2 = min, bit3 = max,
// bit4 = dimension, bit7 = extended display block (u8 ext + desc,
// precision:u8, nominal:f64, alarm_lo/hi:f64; same bit layout as the field
// extended block). Appended after max, before the field count.
inline constexpr uint8_t kUdtHasEnum = 1 << 0;
inline constexpr uint8_t kUdtHasUnit = 1 << 1;
inline constexpr uint8_t kUdtHasMin = 1 << 2;
inline constexpr uint8_t kUdtHasMax = 1 << 3;
inline constexpr uint8_t kUdtHasDimension = 1 << 4;
inline constexpr uint8_t kUdtHasExt = 1 << 7;

inline constexpr size_t kMaxStringLen = 1u << 24;
inline constexpr size_t kMaxCount = 1u << 20;

class Writer {
public:
    void u8(uint8_t t_v) {
        buf_.push_back(static_cast<char>(t_v));
    }
    void u16(uint16_t t_v) {
        for (int i = 0; i < 2; ++i)
            buf_.push_back(static_cast<char>((t_v >> (8 * i)) & 0xFF));
    }
    void u32(uint32_t t_v) {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<char>((t_v >> (8 * i)) & 0xFF));
    }
    void i32(int32_t t_v) {
        u32(static_cast<uint32_t>(t_v));
    }
    void f64(double t_v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &t_v, sizeof(bits));
        for (int i = 0; i < 8; ++i)
            buf_.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
    }
    void str(const std::string& t_s) {
        u32(static_cast<uint32_t>(t_s.size()));
        buf_.append(t_s);
    }
    std::string finish() {
        return std::move(buf_);
    }

private:
    std::string buf_;
};

class Reader {
public:
    explicit Reader(std::string_view t_bytes)
        : bytes_(t_bytes) {
    }
    bool u8(uint8_t& t_v) {
        if (pos_ + 1 > bytes_.size())
            return false;
        t_v = static_cast<uint8_t>(bytes_[pos_]);
        pos_ += 1;
        return true;
    }
    bool u16(uint16_t& t_v) {
        if (pos_ + 2 > bytes_.size())
            return false;
        t_v = static_cast<uint16_t>(static_cast<uint8_t>(bytes_[pos_]) | (static_cast<uint8_t>(bytes_[pos_ + 1]) << 8));
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t& t_v) {
        if (pos_ + 4 > bytes_.size())
            return false;
        t_v = 0;
        for (int i = 0; i < 4; ++i)
            t_v |= static_cast<uint32_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
        pos_ += 4;
        return true;
    }
    bool i32(int32_t& t_v) {
        uint32_t u = 0;
        if (!u32(u))
            return false;
        std::memcpy(&t_v, &u, sizeof(t_v));
        return true;
    }
    bool f64(double& t_v) {
        if (pos_ + 8 > bytes_.size())
            return false;
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>(static_cast<uint8_t>(bytes_[pos_ + i])) << (8 * i);
        std::memcpy(&t_v, &bits, sizeof(t_v));
        pos_ += 8;
        return true;
    }
    bool str(std::string& t_s) {
        uint32_t len = 0;
        if (!u32(len) || len > kMaxStringLen || pos_ + len > bytes_.size())
            return false;
        t_s.assign(bytes_.data() + pos_, len);
        pos_ += len;
        return true;
    }
    bool atEnd() const {
        return pos_ == bytes_.size();
    }

private:
    std::string_view bytes_;
    size_t pos_{0};
};

inline uint8_t endianToU8(s7codec::Endian t_e) {
    switch (t_e) {
        case s7codec::Endian::Little:
            return 1;
        case s7codec::Endian::Unknown:
            return 2;
        default:
            return 0;
    }
}

inline bool u8ToEndian(uint8_t t_v, s7codec::Endian& t_e) {
    switch (t_v) {
        case 0:
            t_e = s7codec::Endian::Little;
            return true;
        case 1:
            t_e = s7codec::Endian::Little;
            return true;
        case 2:
            t_e = s7codec::Endian::Unknown;
            return true;
        default:
            return false;
    }
}

void writeField(Writer& t_w, const DbField& t_f) {
    t_w.str(t_f.name);
    t_w.i32(t_f.offset);
    t_w.u8(t_f.bit_index);
    t_w.str(s7codec::s7TypeToString(t_f.type));
    t_w.u32(t_f.count);
    t_w.i32(t_f.array_lower_bound);
    t_w.i32(t_f.array_upper_bound);
    t_w.u32(t_f.string_capacity);
    t_w.u32(t_f.struct_size);

    uint16_t flags = 0;
    if (!t_f.udt_name.empty())
        flags |= kHasUdt;
    if (!t_f.children.empty())
        flags |= kHasChildren;
    if (t_f.unit.has_value())
        flags |= kHasUnit;
    if (t_f.min_val.has_value())
        flags |= kHasMin;
    if (t_f.max_val.has_value())
        flags |= kHasMax;
    if (!t_f.enum_map.empty())
        flags |= kHasEnum;
    if (!t_f.init_value.empty())
        flags |= kHasInit;
    if (t_f.trigger_events)
        flags |= kTriggerEvents;
    if (t_f.is_dynamic)
        flags |= kIsDynamic;
    if (t_f.dimension.has_value())
        flags |= kHasDimension;
    const bool has_ext = t_f.description.has_value() || t_f.precision.has_value() || t_f.nominal.has_value() ||
                         (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value()) || t_f.is_label || t_f.is_transient || t_f.is_read_only;
    if (has_ext)
        flags |= kHasExt;
    flags |= static_cast<uint16_t>(endianToU8(t_f.endianness) << kEndianShift);
    t_w.u16(flags);

    if (!t_f.udt_name.empty())
        t_w.str(t_f.udt_name);
    if (!t_f.children.empty()) {
        t_w.u32(static_cast<uint32_t>(t_f.children.size()));
        for (const auto& child : t_f.children)
            writeField(t_w, child);
    }
    if (t_f.unit.has_value())
        t_w.str(t_f.unit.value());
    if (t_f.min_val.has_value())
        t_w.f64(t_f.min_val.value());
    if (t_f.max_val.has_value())
        t_w.f64(t_f.max_val.value());
    if (!t_f.enum_map.empty()) {
        t_w.u32(static_cast<uint32_t>(t_f.enum_map.size()));
        for (const auto& [key, value] : t_f.enum_map) {
            t_w.i32(key);
            t_w.str(value);
        }
    }
    if (!t_f.init_value.empty())
        t_w.str(t_f.init_value);
    if (t_f.dimension.has_value())
        t_w.str(t_f.dimension.value());
    if (has_ext) {
        uint16_t ext = 0;
        if (t_f.description.has_value())
            ext |= kExtHasDesc;
        if (t_f.precision.has_value())
            ext |= kExtHasPrecision;
        if (t_f.nominal.has_value())
            ext |= kExtHasNominal;
        if (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value())
            ext |= kExtHasAlarm;
        if (t_f.is_label)
            ext |= kExtIsLabel;
        if (t_f.is_transient)
            ext |= kExtIsTransient;
        if (t_f.is_read_only)
            ext |= kExtIsReadOnly;
        t_w.u16(ext);
        if (t_f.description.has_value())
            t_w.str(t_f.description.value());
        if (t_f.precision.has_value())
            t_w.u8(static_cast<uint8_t>(std::clamp(t_f.precision.value(), 0, 255)));
        if (t_f.nominal.has_value())
            t_w.f64(t_f.nominal.value());
        if (t_f.alarm_lo.has_value() && t_f.alarm_hi.has_value()) {
            t_w.f64(t_f.alarm_lo.value());
            t_w.f64(t_f.alarm_hi.value());
        }
    }
}

bool readField(Reader& t_r, DbField& t_f, int t_depth = 0) {
    // Schema trees are shallow; cap recursion against malicious payloads.
    if (t_depth > 64)
        return false;
    uint32_t count = 0;
    uint16_t flags = 0;
    std::string type_name;
    if (!t_r.str(t_f.name) || !t_r.i32(t_f.offset) || !t_r.u8(t_f.bit_index) || !t_r.str(type_name) || !t_r.u32(count) ||
        !t_r.i32(t_f.array_lower_bound) || !t_r.i32(t_f.array_upper_bound) || !t_r.u32(t_f.string_capacity) || !t_r.u32(t_f.struct_size) ||
        !t_r.u16(flags))
        return false;
    if (count > kMaxCount)
        return false;
    t_f.count = count;
    s7codec::Type type = s7codec::Type::Byte;
    if (!s7codec::stringToType(type_name.c_str(), type))
        return false;
    t_f.type = type;
    s7codec::Endian endian = s7codec::Endian::Little;
    if (!u8ToEndian(static_cast<uint8_t>((flags & kEndianMask) >> kEndianShift), endian))
        return false;
    t_f.endianness = endian;
    t_f.trigger_events = (flags & kTriggerEvents) != 0;
    t_f.is_dynamic = (flags & kIsDynamic) != 0;

    if (flags & kHasUdt) {
        if (!t_r.str(t_f.udt_name))
            return false;
    }
    if (flags & kHasChildren) {
        uint32_t n = 0;
        if (!t_r.u32(n) || n > kMaxCount)
            return false;
        t_f.children.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            DbField child;
            if (!readField(t_r, child, t_depth + 1))
                return false;
            t_f.children.push_back(std::move(child));
        }
    }
    if (flags & kHasUnit) {
        std::string unit;
        if (!t_r.str(unit))
            return false;
        t_f.unit = std::move(unit);
    }
    if (flags & kHasMin) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_f.min_val = v;
    }
    if (flags & kHasMax) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_f.max_val = v;
    }
    if (flags & kHasEnum) {
        uint32_t n = 0;
        if (!t_r.u32(n) || n > kMaxCount)
            return false;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t key = 0;
            std::string value;
            if (!t_r.i32(key) || !t_r.str(value))
                return false;
            t_f.enum_map[key] = std::move(value);
        }
    }
    if (flags & kHasInit) {
        if (!t_r.str(t_f.init_value))
            return false;
    }
    if (flags & kHasDimension) {
        std::string dimension;
        if (!t_r.str(dimension))
            return false;
        t_f.dimension = std::move(dimension);
    }
    if (flags & kHasExt) {
        uint16_t ext = 0;
        if (!t_r.u16(ext))
            return false;
        if (ext & kExtHasDesc) {
            std::string description;
            if (!t_r.str(description))
                return false;
            t_f.description = std::move(description);
        }
        if (ext & kExtHasPrecision) {
            uint8_t precision = 0;
            if (!t_r.u8(precision))
                return false;
            t_f.precision = static_cast<int>(precision);
        }
        if (ext & kExtHasNominal) {
            double nominal = 0.0;
            if (!t_r.f64(nominal))
                return false;
            t_f.nominal = nominal;
        }
        if (ext & kExtHasAlarm) {
            double lo = 0.0, hi = 0.0;
            if (!t_r.f64(lo) || !t_r.f64(hi))
                return false;
            t_f.alarm_lo = lo;
            t_f.alarm_hi = hi;
        }
        t_f.is_label = (ext & kExtIsLabel) != 0;
        t_f.is_transient = (ext & kExtIsTransient) != 0;
        t_f.is_read_only = (ext & kExtIsReadOnly) != 0;
    }
    return true;
}

void writeDb(Writer& t_w, const DbSchema& t_db) {
    t_w.u16(t_db.db_number);
    t_w.str(t_db.db_name);
    t_w.i32(t_db.size_bytes);
    t_w.i32(t_db.max_depth);
    t_w.u8(endianToU8(t_db.endianness));
    t_w.u8(t_db.trigger_events ? 1 : 0);
    t_w.u8(static_cast<uint8_t>(t_db.modbus_area));
    uint8_t flags = t_db.source_file.empty() ? 0 : kDbHasSourceFile;
    t_w.u8(flags);
    if (!t_db.source_file.empty())
        t_w.str(t_db.source_file);
    t_w.u32(static_cast<uint32_t>(t_db.fields.size()));
    for (const auto& field : t_db.fields)
        writeField(t_w, field);
}

bool readDb(Reader& t_r, DbSchema& t_db) {
    uint16_t number = 0;
    uint8_t endian = 0;
    uint8_t trigger = 0;
    uint8_t area = 0;
    uint8_t flags = 0;
    if (!t_r.u16(number) || !t_r.str(t_db.db_name) || !t_r.i32(t_db.size_bytes) || !t_r.i32(t_db.max_depth) || !t_r.u8(endian) ||
        !t_r.u8(trigger) || !t_r.u8(area) || !t_r.u8(flags))
        return false;
    if (!u8ToEndian(endian, t_db.endianness) || area > static_cast<uint8_t>(ModbusArea::Discrete))
        return false;
    t_db.db_number = number;
    t_db.trigger_events = trigger != 0;
    t_db.modbus_area = static_cast<ModbusArea>(area);
    // NOTE: source_file precedes the field count in the byte order.
    if (flags & kDbHasSourceFile) {
        if (!t_r.str(t_db.source_file))
            return false;
    }
    uint32_t n = 0;
    if (!t_r.u32(n) || n > kMaxCount)
        return false;
    t_db.fields.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        DbField field;
        if (!readField(t_r, field))
            return false;
        t_db.fields.push_back(std::move(field));
    }
    return true;
}

void writeUdt(Writer& t_w, const UdtDefinition& t_udt) {
    t_w.u16(t_udt.udt_number);
    t_w.str(t_udt.name);
    t_w.i32(t_udt.size_bytes);
    t_w.i32(t_udt.max_depth);
    t_w.u8(endianToU8(t_udt.endianness));
    t_w.u8(t_udt.trigger_events ? 1 : 0);
    t_w.u8(t_udt.is_scalar_alias ? 1 : 0);
    t_w.str(s7codec::s7TypeToString(t_udt.scalar_type));
    uint8_t flags = 0;
    if (!t_udt.enum_map.empty())
        flags |= kUdtHasEnum;
    if (t_udt.unit.has_value())
        flags |= kUdtHasUnit;
    if (t_udt.min_val.has_value())
        flags |= kUdtHasMin;
    if (t_udt.max_val.has_value())
        flags |= kUdtHasMax;
    if (t_udt.dimension.has_value())
        flags |= kUdtHasDimension;
    const bool udt_has_ext = t_udt.description.has_value() || t_udt.precision.has_value() || t_udt.nominal.has_value() ||
                             (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value());
    if (udt_has_ext)
        flags |= kUdtHasExt;
    t_w.u8(flags);
    if (!t_udt.enum_map.empty()) {
        t_w.u32(static_cast<uint32_t>(t_udt.enum_map.size()));
        for (const auto& [key, value] : t_udt.enum_map) {
            t_w.i32(key);
            t_w.str(value);
        }
    }
    if (t_udt.unit.has_value())
        t_w.str(t_udt.unit.value());
    if (t_udt.min_val.has_value())
        t_w.f64(t_udt.min_val.value());
    if (t_udt.max_val.has_value())
        t_w.f64(t_udt.max_val.value());
    if (t_udt.dimension.has_value())
        t_w.str(t_udt.dimension.value());
    if (udt_has_ext) {
        // Same bit layout as the field extended block (kExt* constants).
        uint16_t ext = 0;
        if (t_udt.description.has_value())
            ext |= kExtHasDesc;
        if (t_udt.precision.has_value())
            ext |= kExtHasPrecision;
        if (t_udt.nominal.has_value())
            ext |= kExtHasNominal;
        if (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value())
            ext |= kExtHasAlarm;
        t_w.u16(ext);
        if (t_udt.description.has_value())
            t_w.str(t_udt.description.value());
        if (t_udt.precision.has_value())
            t_w.u8(static_cast<uint8_t>(std::clamp(t_udt.precision.value(), 0, 255)));
        if (t_udt.nominal.has_value())
            t_w.f64(t_udt.nominal.value());
        if (t_udt.alarm_lo.has_value() && t_udt.alarm_hi.has_value()) {
            t_w.f64(t_udt.alarm_lo.value());
            t_w.f64(t_udt.alarm_hi.value());
        }
    }
    t_w.u32(static_cast<uint32_t>(t_udt.fields.size()));
    for (const auto& field : t_udt.fields)
        writeField(t_w, field);
}

bool readUdt(Reader& t_r, UdtDefinition& t_udt) {
    uint16_t number = 0;
    uint8_t endian = 0;
    uint8_t trigger = 0;
    uint8_t alias = 0;
    uint8_t flags = 0;
    int32_t size_bytes = 0;
    int32_t max_depth = 0;
    std::string scalar_name;
    if (!t_r.u16(number) || !t_r.str(t_udt.name) || !t_r.i32(size_bytes) || !t_r.i32(max_depth) || !t_r.u8(endian) || !t_r.u8(trigger) ||
        !t_r.u8(alias) || !t_r.str(scalar_name) || !t_r.u8(flags))
        return false;
    if (!u8ToEndian(endian, t_udt.endianness))
        return false;
    t_udt.udt_number = number;
    t_udt.size_bytes = static_cast<uint32_t>(size_bytes);
    t_udt.max_depth = static_cast<uint8_t>(max_depth);
    t_udt.trigger_events = trigger != 0;
    t_udt.is_scalar_alias = alias != 0;
    if (!s7codec::stringToType(scalar_name.c_str(), t_udt.scalar_type))
        return false;
    if (flags & kUdtHasEnum) {
        uint32_t m = 0;
        if (!t_r.u32(m) || m > kMaxCount)
            return false;
        for (uint32_t i = 0; i < m; ++i) {
            int32_t key = 0;
            std::string value;
            if (!t_r.i32(key) || !t_r.str(value))
                return false;
            t_udt.enum_map[key] = std::move(value);
        }
    }
    if (flags & kUdtHasUnit) {
        std::string unit;
        if (!t_r.str(unit))
            return false;
        t_udt.unit = std::move(unit);
    }
    if (flags & kUdtHasMin) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_udt.min_val = v;
    }
    if (flags & kUdtHasMax) {
        double v = 0;
        if (!t_r.f64(v))
            return false;
        t_udt.max_val = v;
    }
    if (flags & kUdtHasDimension) {
        std::string dimension;
        if (!t_r.str(dimension))
            return false;
        t_udt.dimension = std::move(dimension);
    }
    if (flags & kUdtHasExt) {
        uint16_t ext = 0;
        if (!t_r.u16(ext))
            return false;
        if (ext & kExtHasDesc) {
            std::string description;
            if (!t_r.str(description))
                return false;
            t_udt.description = std::move(description);
        }
        if (ext & kExtHasPrecision) {
            uint8_t precision = 0;
            if (!t_r.u8(precision))
                return false;
            t_udt.precision = static_cast<int>(precision);
        }
        if (ext & kExtHasNominal) {
            double nominal = 0.0;
            if (!t_r.f64(nominal))
                return false;
            t_udt.nominal = nominal;
        }
        if (ext & kExtHasAlarm) {
            double lo = 0.0, hi = 0.0;
            if (!t_r.f64(lo) || !t_r.f64(hi))
                return false;
            t_udt.alarm_lo = lo;
            t_udt.alarm_hi = hi;
        }
    }
    // Fields come last so a truncated payload fails before mutating much.
    // (The store itself is only committed by the caller on full success.)
    uint32_t n = 0;
    if (!t_r.u32(n) || n > kMaxCount)
        return false;
    std::vector<DbField> fields;
    fields.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        DbField field;
        if (!readField(t_r, field))
            return false;
        fields.push_back(std::move(field));
    }
    t_udt.fields = std::move(fields);
    return true;
}

void writeTag(Writer& t_w, const PlcTag& t_tag) {
    t_w.str(t_tag.name);
    t_w.str(t_tag.table_name);
    t_w.str(t_tag.type_str);
    t_w.str(t_tag.remark);
    t_w.i32(t_tag.addr.area);
    t_w.u16(t_tag.addr.db_number);
    t_w.i32(t_tag.addr.byte_offset);
    t_w.i32(t_tag.addr.bit_index);
    t_w.i32(t_tag.addr.word_len);
    t_w.i32(t_tag.addr.byte_count);
    t_w.str(t_tag.addr.label);
    t_w.str(s7codec::s7TypeToString(t_tag.type));
    t_w.str(t_tag.udt_name);
}

bool readTag(Reader& t_r, PlcTag& t_tag) {
    int32_t area = 0;
    int32_t byte_offset = 0;
    int32_t bit_index = 0;
    int32_t word_len = 0;
    int32_t byte_count = 0;
    uint16_t db_number = 0;
    std::string type_name;
    if (!t_r.str(t_tag.name) || !t_r.str(t_tag.table_name) || !t_r.str(t_tag.type_str) || !t_r.str(t_tag.remark) || !t_r.i32(area) ||
        !t_r.u16(db_number) || !t_r.i32(byte_offset) || !t_r.i32(bit_index) || !t_r.i32(word_len) || !t_r.i32(byte_count) ||
        !t_r.str(t_tag.addr.label) || !t_r.str(type_name) || !t_r.str(t_tag.udt_name))
        return false;
    t_tag.addr.area = area;
    t_tag.addr.db_number = db_number;
    t_tag.addr.byte_offset = byte_offset;
    t_tag.addr.bit_index = bit_index;
    t_tag.addr.word_len = word_len;
    t_tag.addr.byte_count = byte_count;
    if (!s7codec::stringToType(type_name.c_str(), t_tag.type))
        return false;
    return true;
}

} // namespace bin

sgrn::Result<std::string, scl::SclError> SchemaSerializer::serializeBinary(const PlcSchemaStore& t_registry) {
    bin::Writer w;
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[0]));
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[1]));
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[2]));
    w.u8(static_cast<uint8_t>(kBinarySchemaMagic[3]));
    w.u16(kBinarySchemaCodecVersion);
    w.u16(0); // flags, reserved
    w.u32(static_cast<uint32_t>(t_registry.dbs().size()));
    w.u32(static_cast<uint32_t>(t_registry.udts().size()));
    w.u32(static_cast<uint32_t>(t_registry.tags().size()));
    for (const auto& [num, db] : t_registry.dbs()) {
        (void)num;
        bin::writeDb(w, db);
    }
    for (const auto& udt : t_registry.udts())
        bin::writeUdt(w, udt);
    for (const auto& [name, tag] : t_registry.tags()) {
        (void)name;
        bin::writeTag(w, tag);
    }
    return w.finish();
}

sgrn::Result<void, scl::SclError> SchemaSerializer::deserializeBinary(PlcSchemaStore& t_registry, std::string_view t_bytes) {
    bin::Reader r(t_bytes);
    for (char c : kBinarySchemaMagic) {
        uint8_t v = 0;
        if (!r.u8(v) || v != static_cast<uint8_t>(c))
            return scl::SclError::ParseError;
    }
    uint16_t codec_ver = 0;
    uint16_t flags = 0;
    uint32_t db_count = 0;
    uint32_t udt_count = 0;
    uint32_t tag_count = 0;
    if (!r.u16(codec_ver) || !r.u16(flags) || !r.u32(db_count) || !r.u32(udt_count) || !r.u32(tag_count))
        return scl::SclError::ParseError;
    if (codec_ver != kBinarySchemaCodecVersion)
        return scl::SclError::UnsupportedType;
    if (flags != 0)
        return scl::SclError::UnsupportedType;
    if (db_count > bin::kMaxCount || udt_count > bin::kMaxCount || tag_count > bin::kMaxCount)
        return scl::SclError::OutOfRange;

    // Parse into locals first; the registry is only committed below, mirroring
    // deserialize()'s structure (direct dbs_ insert, validated UDT/tag adds).
    std::vector<DbSchema> dbs;
    dbs.reserve(db_count);
    for (uint32_t i = 0; i < db_count; ++i) {
        DbSchema db;
        if (!bin::readDb(r, db))
            return scl::SclError::ParseError;
        dbs.push_back(std::move(db));
    }
    std::vector<UdtDefinition> udts;
    udts.reserve(udt_count);
    for (uint32_t i = 0; i < udt_count; ++i) {
        UdtDefinition udt;
        if (!bin::readUdt(r, udt))
            return scl::SclError::ParseError;
        udts.push_back(std::move(udt));
    }
    std::vector<PlcTag> tags;
    tags.reserve(tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        PlcTag tag;
        if (!bin::readTag(r, tag))
            return scl::SclError::ParseError;
        tags.push_back(std::move(tag));
    }
    if (!r.atEnd())
        return scl::SclError::ParseError;

    for (auto& db : dbs)
        t_registry.dbs_[db.db_number] = std::move(db);
    for (auto& udt : udts) {
        if (auto res = t_registry.addUdt(std::move(udt)); res.hasError())
            return res.error();
    }
    for (auto& tag : tags) {
        if (auto res = t_registry.addTag(std::move(tag)); res.hasError())
            return res.error();
    }

    t_registry.rebuildIndices();
    SchemaSerializer::resolveUdtsInRegistry(t_registry);
    t_registry.rebuildIndices();
    return {};
}

} // namespace sgrn::scl
