#pragma once

#include <sgrn/gateway/twin/PlcMemory.hpp>

namespace sgrn::gateway::adapters::ethernetip
{

enum class EipAdapterError : uint8_t {
    NULL_POINTER,
    PATH_SEGMENT_ERROR,
    TOO_MUCH_DATA,
    NOT_ENOUGH_DATA,
    TYPE_MISMATCH,
    ATTRIBUTE_NOT_FOUND,
    WRITE_FAILED,
    SERVER_CREATE_FAILED,
    NOT_INITIALIZED,
    INTERNAL,
};

/// Static translator: EipAdapterError -> human-readable message.
inline const char* toString(EipAdapterError t_err) {
    switch (t_err) {
        case EipAdapterError::NULL_POINTER:
            return "null pointer while handling CIP request";
        case EipAdapterError::PATH_SEGMENT_ERROR:
            return "CIP path segment does not resolve to a twin field";
        case EipAdapterError::TOO_MUCH_DATA:
            return "payload exceeds attribute capacity";
        case EipAdapterError::NOT_ENOUGH_DATA:
            return "payload shorter than the attribute size";
        case EipAdapterError::TYPE_MISMATCH:
            return "CIP type does not match the field type";
        case EipAdapterError::ATTRIBUTE_NOT_FOUND:
            return "attribute not registered in the instance map";
        case EipAdapterError::WRITE_FAILED:
            return "failed to write decoded CIP value into PLC memory";
        case EipAdapterError::SERVER_CREATE_FAILED:
            return "failed to create the EtherNet/IP server";
        case EipAdapterError::NOT_INITIALIZED:
            return "twin state not attached yet";
        case EipAdapterError::INTERNAL:
            return "internal EtherNet/IP adapter error";
    }
    return "unknown EtherNet/IP adapter error";
}

// -----------------------------------------------------------------------------
// Wire translators. Groupings mirror the OPC UA adapter's toUAStatusCode():
//   - PLC_STATE_NOT_INITIALIZED -> 0x01 (connection failure / not reachable)
//   - DB_SEGMENT_NOT_FOUND / UNMAPPED_ARENA_REGION -> 0x15 (path segment error)
//   - RANGE_* -> 0x0F (too much data requested)
//   - NULL_BUFFER / INVALID_BIT_INDEX -> 0x1F (vendor-specific / internal)
// -----------------------------------------------------------------------------

/// PlcMemoryError -> CIP general-status byte (0x00–0x1F).
constexpr uint8_t toCipStatus(sgrn::gateway::twin::PlcMemoryError t_status) {
    using sgrn::gateway::twin::PlcMemoryError;
    switch (t_status) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
            return 0x01; // connection failure / device not reachable
        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
        case PlcMemoryError::UNMAPPED_ARENA_REGION:
            return 0x15; // path segment error (target does not exist)
        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
            return 0x0F; // too much data requested
        case PlcMemoryError::NULL_BUFFER:
        case PlcMemoryError::INVALID_BIT_INDEX:
            return 0x1F; // vendor-specific / internal error
    }
    return 0x1F;
}

/// EipAdapterError -> CIP general-status byte (0x00–0x1F).
constexpr uint8_t toCipStatus(EipAdapterError t_err) {
    switch (t_err) {
        case EipAdapterError::PATH_SEGMENT_ERROR:
        case EipAdapterError::ATTRIBUTE_NOT_FOUND:
            return 0x14; // attribute not supported / bad path
        case EipAdapterError::TOO_MUCH_DATA:
            return 0x0F; // too much data requested
        case EipAdapterError::NOT_ENOUGH_DATA:
            return 0x05; // not enough data
        case EipAdapterError::TYPE_MISMATCH:
            return 0x13; // invalid parameter
        case EipAdapterError::WRITE_FAILED:
            return 0x0C; // device state conflict
        case EipAdapterError::SERVER_CREATE_FAILED:
        case EipAdapterError::NULL_POINTER:
        case EipAdapterError::INTERNAL:
            return 0x1F; // vendor-specific / internal error
        case EipAdapterError::NOT_INITIALIZED:
            return 0x01; // connection failure
    }
    return 0x1F;
}

} // namespace sgrn::gateway::adapters::ethernetip
