#pragma once

#include <sgrn/scl/types/modbus/ModbusVirtualEntry.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <string>
#include <vector>

namespace sgrn::scl
{

// ---------------------------------------------------------------------------
// Virtual map — all four address spaces
// ---------------------------------------------------------------------------

struct ModbusVirtualMap {
    std::vector<ModbusVirtualEntry> holding;  ///< 4x space
    std::vector<ModbusVirtualEntry> input;    ///< 3x space
    std::vector<ModbusVirtualEntry> coil;     ///< 0x space
    std::vector<ModbusVirtualEntry> discrete; ///< 1x space

    /// Sizes for modbus_mapping_t allocation
    int total_holding{0};
    int total_input{0};
    int total_coils{0};
    int total_discrete{0};

    /// Diagnostic warnings (alignment, empty areas, etc.)
    std::vector<std::string> warnings;

    bool empty() const {
        return holding.empty() && input.empty() && coil.empty() && discrete.empty();
    }
};

} // namespace sgrn::scl

namespace sgrn::scl::modbus::map
{
/// Writer-agnostic JSON "form" for the full virtual Modbus map.
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::ModbusVirtualMap& t_map) {
    t_writer.startObject();

    t_writer.key("holding");
    t_writer.startArray();
    for (const auto& t_e : t_map.holding)
        sgrn::scl::modbus::entry::serializeToWriter(t_writer, t_e);
    t_writer.endArray();

    t_writer.key("input");
    t_writer.startArray();
    for (const auto& t_e : t_map.input)
        sgrn::scl::modbus::entry::serializeToWriter(t_writer, t_e);
    t_writer.endArray();

    t_writer.key("coil");
    t_writer.startArray();
    for (const auto& t_e : t_map.coil)
        sgrn::scl::modbus::entry::serializeToWriter(t_writer, t_e);
    t_writer.endArray();

    t_writer.key("discrete");
    t_writer.startArray();
    for (const auto& t_e : t_map.discrete)
        sgrn::scl::modbus::entry::serializeToWriter(t_writer, t_e);
    t_writer.endArray();

    t_writer.key("total_holding");
    t_writer.int32(t_map.total_holding);
    t_writer.key("total_input");
    t_writer.int32(t_map.total_input);
    t_writer.key("total_coils");
    t_writer.int32(t_map.total_coils);
    t_writer.key("total_discrete");
    t_writer.int32(t_map.total_discrete);

    if (!t_map.warnings.empty()) {
        t_writer.key("warnings");
        t_writer.startArray();
        for (const auto& warn : t_map.warnings)
            t_writer.string(warn.c_str());
        t_writer.endArray();
    }

    t_writer.endObject();
}

/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::ModbusVirtualMap fromJson(const sgrn::json::dom::Value& t_value);
sgrn::scl::ModbusVirtualMap fromJsonString(const std::string& t_value);
} // namespace sgrn::scl::modbus::map

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::ModbusVirtualMap& t_modbus_virtual_map);
sgrn::json::dom::Document toJson(const sgrn::scl::ModbusVirtualMap& t_modbus_virtual_map);
