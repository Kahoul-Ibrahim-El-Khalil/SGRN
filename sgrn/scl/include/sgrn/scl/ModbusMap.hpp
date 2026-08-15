#pragma once

/**
 * @file  ModbusMap.hpp
 * @brief Virtual Modbus register map — built from #MODBUS_* SCL directives.
 *
 * The map is the runtime representation of Appendix G of the thesis.
 * It is built once at gateway startup from the PlcSchemaStore and is
 * immutable thereafter.  All four Modbus address spaces are populated:
 *
 *   Holding registers (4x)  — read-write, 16-bit words
 *   Input   registers (3x)  — read-only,  16-bit words
 *   Coils             (0x)  — read-write, 1-bit
 *   Discrete inputs   (1x)  — read-only,  1-bit
 *
 * Addresses are auto-assigned sequentially within each area, ordered by
 * DB number.  No manual address configuration is required or supported.
 *
 * Padding: fields whose byte_count is not a multiple of 2 occupy one
 * extra register word whose high byte is always zero.  The `padded` flag
 * is exposed in the /registry/modbus endpoint for diagnostic purposes.
 */

#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <string>
#include <vector>

namespace sgrn::scl
{

// ---------------------------------------------------------------------------
// Virtual register entry — one leaf field in the virtual Modbus address space
// ---------------------------------------------------------------------------

struct ModbusVirtualEntry {
    uint16_t db_number{0};
    std::string field_path; ///< dot-separated path inside DB, e.g. "pump_1.setpoint"
    DataType type{DataType::Word};
    int byte_offset{0};    ///< byte offset of field inside DB arena
    int bit_index{0};      ///< bit index inside the byte (only for BOOL fields)
    int byte_count{0};     ///< real field size in bytes
    int reg_start{0};      ///< first register / coil address in virtual map
    int reg_count{0};      ///< number of 16-bit registers, or number of coils/bits
    bool padded{false};    ///< true → byte_count is odd; high byte of last reg is unused
    bool read_only{false}; ///< Input / Discrete areas are read-only
};

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

// ---------------------------------------------------------------------------
// Builder and serializer
// ---------------------------------------------------------------------------

/**
 * @brief Build the virtual Modbus register map from all annotated DBs.
 *
 * Iterates store.dbs() ordered by db_number.  Only DBs with
 * modbus_area != ModbusArea::None are included.  Fields within each DB
 * are flattened (nested structs expanded) and assigned sequential
 * register/coil addresses within their respective area.
 */
ModbusVirtualMap buildModbusVirtualMap(const PlcSchemaStore& t_store);

/**
 * @brief Serialize the virtual map to the /registry/modbus JSON format.
 *
 * Produces Listing G.2 from the thesis:
 * { "holding_registers": [...], "input_registers": [...],
 *   "coils": [...], "discrete_inputs": [...] }
 */
std::string serializeModbusMapToJson(const ModbusVirtualMap& t_map);

} // namespace sgrn::scl
