#pragma once
#include <cstdint>
namespace sgrn::scl
{
// ---------------------------------------------------------------------------
// Modbus exposure area — set by #MODBUS_* block-level directives.
// None = DB is not exposed via Modbus (default).
// ---------------------------------------------------------------------------
enum class ModbusArea : uint8_t {
    None = 0,
    Holding = 1,  ///< 4x — holding registers (FC03 read / FC16 write), read-write
    Input = 2,    ///< 3x — input registers   (FC04 read-only)
    Coil = 3,     ///< 0x — coils             (FC01 read / FC15 write), read-write bits
    Discrete = 4, ///< 1x — discrete inputs   (FC02 read-only bits)
};

} // namespace sgrn::scl
