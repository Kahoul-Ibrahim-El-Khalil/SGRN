#pragma once
#include <sgrn/scl/types/DataType.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <cstdint>
#include <s7codec/codec.hpp>
#include <string>

namespace sgrn::scl
{
// ---------------------------------------------------------------------------
// Virtual register entry — one leaf field in the virtual Modbus address space
// ---------------------------------------------------------------------------

struct ModbusVirtualEntry {
    uint16_t db_number{0};
    std::string field_path; ///< dot-separated path inside DB, e.g. "pump_1.setpoint"
    DataType type{DataType::Word};
    int byte_offset{0};     ///< byte offset of field inside DB arena
    uint8_t bit_index{0};   ///< bit index inside the byte (only for BOOL fields)
    uint32_t byte_count{0}; ///< real field size in bytes
    uint32_t reg_start{0};  ///< first register / coil address in virtual map
    uint32_t reg_count{0};  ///< number of 16-bit registers, or number of coils/bits
    bool padded{false};     ///< true → byte_count is odd; high byte of last reg is unused
    bool read_only{false};  ///< Input / Discrete areas are read-only
};

} // namespace sgrn::scl

namespace sgrn::scl::modbus::entry
{
/// Writer-agnostic JSON "form" for a single virtual Modbus entry.
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::ModbusVirtualEntry& t_e) {
    t_writer.startObject();
    t_writer.key("number");
    t_writer.int32(t_e.db_number);
    t_writer.key("field_path");
    t_writer.string(t_e.field_path.c_str());
    t_writer.key("type");
    t_writer.string(s7codec::s7TypeToString(t_e.type));
    t_writer.key("byte_offset");
    t_writer.int32(t_e.byte_offset);
    t_writer.key("bit");
    t_writer.int32(t_e.bit_index);
    t_writer.key("byte_count");
    t_writer.int32(t_e.byte_count);
    t_writer.key("reg_start");
    t_writer.int32(t_e.reg_start);
    t_writer.key("reg_count");
    t_writer.int32(t_e.reg_count);
    t_writer.key("padded");
    t_writer.boolean(t_e.padded);
    t_writer.key("read_only");
    t_writer.boolean(t_e.read_only);
    t_writer.endObject();
}

/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::ModbusVirtualEntry fromJson(const sgrn::json::dom::Value& t_value);
sgrn::scl::ModbusVirtualEntry fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::modbus::entry

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::ModbusVirtualEntry& t_entry);
sgrn::json::dom::Document toJson(const sgrn::scl::ModbusVirtualEntry& t_entry);
