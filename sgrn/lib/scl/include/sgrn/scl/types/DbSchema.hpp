#pragma once
#include <fmt/format.h>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/scl/types/UdtDefinition.hpp>
#include <sgrn/scl/types/modbus/ModbusArea.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <s7codec/codec.hpp>
#include <stdexcept>
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
    /// Block-scoped byte order; Big is canonical (see DbField::endianness).
    s7codec::Endian endianness{s7codec::Endian::Little};
    bool trigger_events{false};
    ModbusArea modbus_area{ModbusArea::None}; ///< Modbus exposure directive
    uint64_t dataset_step_ns{0};              ///< Default dataset time step in nanoseconds (#DATASET_STEP)
};

} // namespace sgrn::scl

namespace sgrn::scl::db
{
/// Writer-agnostic JSON "form" for DbSchema. When t_headers_only is true only
/// the DB header is emitted (fields are omitted).
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::DbSchema& t_db, bool t_headers_only = false) {
    t_writer.startObject();
    t_writer.key("number");
    t_writer.int32(t_db.db_number);
    t_writer.key("name");
    t_writer.string(t_db.db_name.c_str());
    t_writer.key("size_bytes");
    t_writer.int32(t_db.size_bytes);
    if (!t_db.source_file.empty()) {
        t_writer.key("source_file");
        t_writer.string(t_db.source_file.c_str());
    }
    if (t_db.endianness != s7codec::Endian::Little) {
        t_writer.key("endianness");
        t_writer.string(t_db.endianness == s7codec::Endian::Little ? "little" : "big");
    }
    if (t_db.trigger_events) {
        t_writer.key("trigger_events");
        t_writer.boolean(true);
    }
    if (t_db.modbus_area != ModbusArea::None) {
        t_writer.key("modbus_area");
        switch (t_db.modbus_area) {
            case ModbusArea::Holding:
                t_writer.string("holding");
                break;
            case ModbusArea::Input:
                t_writer.string("input");
                break;
            case ModbusArea::Coil:
                t_writer.string("coil");
                break;
            case ModbusArea::Discrete:
                t_writer.string("discrete");
                break;
            default:
                break;
        }
    }
    if (t_db.dataset_step_ns > 0) {
        t_writer.key("dataset_step_ns");
        t_writer.uint64(t_db.dataset_step_ns);
    }
    if (!t_headers_only) {
        t_writer.key("fields");
        t_writer.startArray();
        for (const auto& t_field : t_db.fields) {
            sgrn::scl::field::serializeToWriter(t_writer, t_field);
        }
        t_writer.endArray();
    }
    t_writer.endObject();
}

/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::DbSchema fromJson(const sgrn::json::dom::Value& t_value);
sgrn::scl::DbSchema fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::db

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::DbSchema& t_db);
sgrn::json::dom::Document toJson(const sgrn::scl::DbSchema& t_db);

template <>
struct fmt::formatter<sgrn::scl::DbSchema> : formatter<std::string_view> {
    auto format(const sgrn::scl::DbSchema& t_db, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("DbSchema{{db_number={}, db_name=\"{}\", size_bytes={}, fields={}, source_file=\"{}\"}}", t_db.db_number,
                t_db.db_name, t_db.size_bytes, t_db.fields.size(), t_db.source_file),
            t_ctx);
    }
};
