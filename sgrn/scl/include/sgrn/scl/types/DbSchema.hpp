#pragma once
#include <fmt/core.h>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/scl/types/UdtDefinition.hpp>
#include <sgrn/scl/types/modbus/ModbusArea.hpp>
#include <rapidjson/document.h>
#include <s7codec/codec.hpp>
#include <string>
#include <vector>

namespace sgrn::scl
{

struct DbSchema {
    uint16_t db_number{0};
    std::string db_name;
    int size_bytes{0};
    int max_depth{0};
    std::vector<DbField> fields;

    std::string source_file;
    s7codec::Endian endianness{s7codec::Endian::Big};
    bool trigger_events{false};
    ModbusArea modbus_area{ModbusArea::None}; ///< Modbus exposure directive
};

} // namespace sgrn::scl

namespace sgrn::scl::db
{
/// Writer-agnostic JSON "form" for DbSchema. When t_headers_only is true only
/// the DB header is emitted (fields are omitted).
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::DbSchema& t_db, bool t_headers_only = false) {
    t_writer.StartObject();
    t_writer.Key("number");
    t_writer.Int(t_db.db_number);
    t_writer.Key("name");
    t_writer.String(t_db.db_name.c_str());
    t_writer.Key("size_bytes");
    t_writer.Int(t_db.size_bytes);
    if (!t_db.source_file.empty()) {
        t_writer.Key("source_file");
        t_writer.String(t_db.source_file.c_str());
    }
    if (t_db.endianness != s7codec::Endian::Big) {
        t_writer.Key("endianness");
        t_writer.String(t_db.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_db.trigger_events) {
        t_writer.Key("trigger_events");
        t_writer.Bool(true);
    }
    if (t_db.modbus_area != ModbusArea::None) {
        t_writer.Key("modbus_area");
        switch (t_db.modbus_area) {
            case ModbusArea::Holding:
                t_writer.String("holding");
                break;
            case ModbusArea::Input:
                t_writer.String("input");
                break;
            case ModbusArea::Coil:
                t_writer.String("coil");
                break;
            case ModbusArea::Discrete:
                t_writer.String("discrete");
                break;
            default:
                break;
        }
    }
    if (!t_headers_only) {
        t_writer.Key("fields");
        t_writer.StartArray();
        for (const auto& t_field : t_db.fields) {
            sgrn::scl::field::serializeToWriter(t_writer, t_field);
        }
        t_writer.EndArray();
    }
    t_writer.EndObject();
}

/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::DbSchema fromJson(const rapidjson::Value& t_value);
sgrn::scl::DbSchema fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::db

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::DbSchema& t_db);
rapidjson::Document toJson(const sgrn::scl::DbSchema& t_db);

template <>
struct fmt::formatter<sgrn::scl::DbSchema> : formatter<std::string_view> {
    auto format(const sgrn::scl::DbSchema& t_db, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("DbSchema{{db_number={}, db_name=\"{}\", size_bytes={}, fields={}, source_file=\"{}\"}}", t_db.db_number,
                t_db.db_name, t_db.size_bytes, t_db.fields.size(), t_db.source_file),
            t_ctx);
    }
};
