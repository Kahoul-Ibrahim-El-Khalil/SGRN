#pragma once

#include <sgrn/gateway/twin/PlcMemory.hpp>

namespace sgrn::gateway::adapters::modbus
{

/**
 * @brief Error outcomes that can flow out of the Modbus TCP slave adapter.
 *
 * Fallible operations inside the adapter return this enum via sgrn::Result so
 * failures cascade upward machine-checkably and are translated to a Modbus
 * exception code exactly once — at the MBAP response-building boundary.
 * toString() mirrors the OPC UA adapter's errors.hpp pattern.
 */
enum class ModbusAdapterError {
    NULL_POINTER,
    SCHEMA_NOT_FOUND,
    ILLEGAL_ADDRESS,
    OUT_OF_RANGE,
    SERVER_DEVICE_FAILURE,
    SOCKET_CREATE_FAILED,
    MAPPING_CREATE_FAILED,
    NOT_INITIALIZED,
    GATEWAY_TARGET_FAILED,
};

/// Static translator: ModbusAdapterError -> human-readable message.
inline const char* toString(ModbusAdapterError t_err) {
    switch (t_err) {
        case ModbusAdapterError::NULL_POINTER:
            return "null pointer while handling Modbus request";
        case ModbusAdapterError::SCHEMA_NOT_FOUND:
            return "no schema entry backing the requested map area";
        case ModbusAdapterError::ILLEGAL_ADDRESS:
            return "illegal data address";
        case ModbusAdapterError::OUT_OF_RANGE:
            return "quantity or range exceeds the mapped space";
        case ModbusAdapterError::SERVER_DEVICE_FAILURE:
            return "server device failure";
        case ModbusAdapterError::SOCKET_CREATE_FAILED:
            return "failed to create TCP listening socket";
        case ModbusAdapterError::MAPPING_CREATE_FAILED:
            return "failed to allocate modbus mapping";
        case ModbusAdapterError::NOT_INITIALIZED:
            return "twin state not attached yet";
        case ModbusAdapterError::GATEWAY_TARGET_FAILED:
            return "gateway target device failed to respond";
    }
    return "unknown Modbus adapter error";
}

// -----------------------------------------------------------------------------
// Wire translators. Groupings mirror the OPC UA adapter's toUAStatusCode():
//   - PLC_STATE_NOT_INITIALIZED -> 0x04 (server device failure; connectivity)
//   - DB_SEGMENT_NOT_FOUND / UNMAPPED_ARENA_REGION -> 0x02 (illegal data addr)
//   - RANGE_* / internal -> 0x04 (server device failure)
// Modbus has no richer vocabulary than 0x01–0x04 (FC03/FC04 style), so several
// internal classes collapse onto 0x04 by design.
// -----------------------------------------------------------------------------

/// PlcMemoryError -> Modbus exception code (0x01–0x04).
constexpr uint8_t toExceptionCode(sgrn::gateway::twin::PlcMemoryError t_status) {
    using sgrn::gateway::twin::PlcMemoryError;
    switch (t_status) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
            return 0x04; // server device failure
        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
        case PlcMemoryError::UNMAPPED_ARENA_REGION:
            return 0x02; // illegal data address
        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
            return 0x04; // server device failure (cannot honor the range)
        case PlcMemoryError::NULL_BUFFER:
        case PlcMemoryError::INVALID_BIT_INDEX:
            return 0x04; // server device failure (internal)
    }
    return 0x04;
}

/// ModbusAdapterError -> Modbus exception code (0x01–0x04).
constexpr uint8_t toExceptionCode(ModbusAdapterError t_err) {
    switch (t_err) {
        case ModbusAdapterError::ILLEGAL_ADDRESS:
        case ModbusAdapterError::SCHEMA_NOT_FOUND:
            return 0x02; // illegal data address
        case ModbusAdapterError::OUT_OF_RANGE:
            return 0x03; // illegal data value
        case ModbusAdapterError::GATEWAY_TARGET_FAILED:
            return 0x0A; // gateway target device failed to respond
        case ModbusAdapterError::NULL_POINTER:
        case ModbusAdapterError::SERVER_DEVICE_FAILURE:
        case ModbusAdapterError::SOCKET_CREATE_FAILED:
        case ModbusAdapterError::MAPPING_CREATE_FAILED:
        case ModbusAdapterError::NOT_INITIALIZED:
            return 0x04; // server device failure
    }
    return 0x04;
}

} // namespace sgrn::gateway::adapters::modbus
