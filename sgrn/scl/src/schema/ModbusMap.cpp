/**
 * @file  ModbusMap.cpp
 * @brief Virtual Modbus register map builder — Appendix G implementation.
 *
 * buildModbusVirtualMap() flattens all annotated DBs in the PlcSchemaStore
 * into the four Modbus address spaces.  serializeModbusMapToJson() produces
 * the /registry/modbus JSON response (Listing G.2 from the thesis).
 */

#include <sgrn/scl/functions/modbus.hpp>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <s7codec/types.hpp>

#include <fmt/core.h>
#include <algorithm>

namespace sgrn::scl
{

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail
{

/// Returns the number of 16-bit registers needed to hold byte_count bytes.
/// A field of 1 byte occupies 1 register (padded = true).
static uint32_t bytesToRegCount(uint32_t t_byte_count) {
    return (t_byte_count + 1) / 2;
}

/// Recursively flatten a field tree into a list of (path, DbField) pairs.
/// Arrays are represented as a single entry whose byte_count covers the whole array.
static void flattenFields(
    const std::vector<DbField>& t_fields, const std::string& t_prefix, std::vector<std::pair<std::string, const DbField*>>& t_out) {
    for (const auto& t_f : t_fields) {
        std::string path = t_prefix.empty() ? t_f.name : t_prefix + "." + t_f.name;
        if (t_f.type == DataType::Struct && !t_f.children.empty()) {
            flattenFields(t_f.children, path, t_out);
        } else {
            t_out.emplace_back(path, &t_f);
        }
    }
}

/// Compute total byte_count for a leaf field (handles arrays and strings).
static uint32_t fieldByteCount(const DbField& t_f) {
    if (t_f.type == DataType::Struct)
        return t_f.struct_size * std::max(static_cast<uint32_t>(1), t_f.count);
    if (t_f.type == DataType::Bool && t_f.count <= 1)
        return 1; // single bit occupies 1 byte in the arena
    if (t_f.type == DataType::String || t_f.type == DataType::WString || t_f.type == DataType::XString || t_f.type == DataType::XWString) {
        return s7codec::typeSpanBytes(t_f.type, std::max(static_cast<uint32_t>(1), t_f.count)).value_or(0);
    }
    auto elem_opt = s7codec::primitiveSize(t_f.type);
    if (!elem_opt.has_value() || elem_opt.value() <= 0)
        return 2; // fallback for unknowns
    return static_cast<int>(elem_opt.value()) * std::max(static_cast<uint32_t>(1), t_f.count);
}

/// Build entries for a register-based area (Holding or Input).
static void buildRegisterEntries(
    const DbSchema& t_db, bool t_read_only, int& t_cursor, std::vector<ModbusVirtualEntry>& t_out, std::vector<std::string>& t_warnings) {
    std::vector<std::pair<std::string, const DbField*>> flat;
    flattenFields(t_db.fields, "", flat);

    for (const auto& [path, t_f] : flat) {
        // Skip BOOL scalars — they live in the coil/discrete spaces naturally,
        // but if the engineer put them in a HOLDING DB, they get 1 register each.
        int t_byte_count = fieldByteCount(*t_f);
        if (t_byte_count <= 0) {
            t_warnings.push_back(fmt::format("DB{} field '{}': zero byte count, skipped", t_db.db_number, path));
            continue;
        }

        int reg_count = bytesToRegCount(t_byte_count);
        bool padded = (t_byte_count % 2 != 0);

        ModbusVirtualEntry entry{.db_number = t_db.db_number,
            .field_path = path,
            .type = t_f->type,
            .byte_offset = t_f->offset,
            .byte_count = static_cast<uint32_t>(t_byte_count),
            .reg_start = static_cast<uint32_t>(t_cursor),
            .reg_count = static_cast<uint32_t>(reg_count),
            .padded = padded,
            .read_only = t_read_only};
        t_out.push_back(std::move(entry));

        t_cursor += reg_count;

        if (padded) {
            t_warnings.push_back(fmt::format(
                "DB{} field '{}': odd byte count ({}) — 1 padding byte in register {}", t_db.db_number, path, t_byte_count, t_cursor - 1));
        }
    }
}

/// Build entries for a bit-based area (Coil or Discrete).
static void buildBitEntries(
    const DbSchema& t_db, bool t_read_only, int& t_cursor, std::vector<ModbusVirtualEntry>& t_out, std::vector<std::string>& t_warnings) {
    std::vector<std::pair<std::string, const DbField*>> flat;
    flattenFields(t_db.fields, "", flat);

    for (const auto& [path, t_f] : flat) {
        // In a COIL/DISCRETE DB we only expose BOOL fields as individual bits.
        // Other types are skipped with a warning — the spec says to use
        // dedicated aligned DBs for best practice.
        if (t_f->type != DataType::Bool) {
            t_warnings.push_back(fmt::format("DB{} field '{}': non-BOOL field in coil/discrete area, skipped", t_db.db_number, path));
            continue;
        }

        uint32_t bit_count = std::max(static_cast<uint32_t>(1), t_f->count); // bool array → multiple coils

        ModbusVirtualEntry entry{.db_number = t_db.db_number,
            .field_path = path,
            .type = DataType::Bool,
            .byte_offset = t_f->offset,
            .bit_index = t_f->bit_index,
            .byte_count = (bit_count + 7) / 8, // packed bytes in arena
            .reg_start = static_cast<uint32_t>(t_cursor),
            .reg_count = static_cast<uint32_t>(bit_count), // one coil per bit
            .padded = false,
            .read_only = t_read_only};
        t_out.push_back(std::move(entry));

        t_cursor += bit_count;
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ModbusVirtualMap buildModbusVirtualMap(const PlcSchemaStore& t_store) {
    ModbusVirtualMap vmap;

    // Collect annotated DBs sorted by db_number for deterministic address assignment
    std::vector<const DbSchema*> annotated;
    for (const auto& [num, t_db] : t_store.dbs()) {
        if (t_db.modbus_area != ModbusArea::None)
            annotated.push_back(&t_db);
    }
    std::sort(annotated.begin(), annotated.end(), [](const auto* tp_a, const auto* tp_b) { return tp_a->db_number < tp_b->db_number; });

    if (annotated.empty()) {
        vmap.warnings.push_back("No DBs annotated with #MODBUS_* directives.");
        return vmap;
    }

    int cursor_holding = 0;
    int cursor_input = 0;
    int cursor_coil = 0;
    int cursor_discrete = 0;

    for (const auto* t_db : annotated) {
        switch (t_db->modbus_area) {
            case ModbusArea::Holding:
                detail::buildRegisterEntries(*t_db, false, cursor_holding, vmap.holding, vmap.warnings);
                break;
            case ModbusArea::Input:
                detail::buildRegisterEntries(*t_db, true, cursor_input, vmap.input, vmap.warnings);
                break;
            case ModbusArea::Coil:
                detail::buildBitEntries(*t_db, false, cursor_coil, vmap.coil, vmap.warnings);
                break;
            case ModbusArea::Discrete:
                detail::buildBitEntries(*t_db, true, cursor_discrete, vmap.discrete, vmap.warnings);
                break;
            case ModbusArea::None:
                break;
        }
    }

    vmap.total_holding = cursor_holding;
    vmap.total_input = cursor_input;
    vmap.total_coils = cursor_coil;
    vmap.total_discrete = cursor_discrete;

    return vmap;
}

// ---------------------------------------------------------------------------
// JSON serializer — /registry/modbus  (Listing G.2 format)
// ---------------------------------------------------------------------------

static void writeRegEntry(rapidjson::PrettyWriter<rapidjson::StringBuffer>& t_w, const ModbusVirtualEntry& t_e) {
    t_w.StartObject();
    t_w.Key("start");
    t_w.Int(t_e.reg_start);
    t_w.Key("count");
    t_w.Int(t_e.reg_count);
    t_w.Key("type");
    t_w.String(s7codec::s7TypeToString(t_e.type));
    t_w.Key("access");
    t_w.String(t_e.read_only ? "ro" : "rw");
    t_w.Key("padded");
    t_w.Bool(t_e.padded);
    t_w.Key("source");
    t_w.String(fmt::format("DB{}.{}", t_e.db_number, t_e.field_path).c_str());
    t_w.EndObject();
}

static void writeBitEntry(rapidjson::PrettyWriter<rapidjson::StringBuffer>& t_w, const ModbusVirtualEntry& t_e) {
    t_w.StartObject();
    t_w.Key("address");
    t_w.Int(t_e.reg_start);
    t_w.Key("bit_index");
    t_w.Int(t_e.bit_index);
    t_w.Key("type");
    t_w.String("BOOL");
    t_w.Key("access");
    t_w.String(t_e.read_only ? "ro" : "rw");
    t_w.Key("padded");
    t_w.Bool(false);
    t_w.Key("source");
    t_w.String(fmt::format("DB{}.{}", t_e.db_number, t_e.field_path).c_str());
    t_w.EndObject();
}

std::string serializeModbusMapToJson(const ModbusVirtualMap& t_map) {
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> t_w(sb);

    t_w.StartObject();

    t_w.Key("holding_registers");
    t_w.StartArray();
    for (const auto& t_e : t_map.holding)
        writeRegEntry(t_w, t_e);
    t_w.EndArray();

    t_w.Key("input_registers");
    t_w.StartArray();
    for (const auto& t_e : t_map.input)
        writeRegEntry(t_w, t_e);
    t_w.EndArray();

    t_w.Key("coils");
    t_w.StartArray();
    for (const auto& t_e : t_map.coil)
        writeBitEntry(t_w, t_e);
    t_w.EndArray();

    t_w.Key("discrete_inputs");
    t_w.StartArray();
    for (const auto& t_e : t_map.discrete)
        writeBitEntry(t_w, t_e);
    t_w.EndArray();

    if (!t_map.warnings.empty()) {
        t_w.Key("warnings");
        t_w.StartArray();
        for (const auto& warn : t_map.warnings)
            t_w.String(warn.c_str());
        t_w.EndArray();
    }

    t_w.EndObject();
    return sb.GetString();
}

} // namespace sgrn::scl
