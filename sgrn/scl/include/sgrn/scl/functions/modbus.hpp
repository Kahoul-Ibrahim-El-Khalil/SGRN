#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types/modbus/ModbusVirtualMap.hpp>
#include <string>
namespace sgrn::scl
{
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
