#pragma once

#include <open62541/types.h>

namespace sgrn::gateway::adapters
{

/**
 * @brief Error outcomes that can flow out of the OPC UA <-> S7 encode/decode
 *        layer in the protocol adapter.
 *
 * Every Result<..., ...> in the opcua adapter (encoders.hpp / decoders.hpp)
 * returns this enum instead of a std::string so that the failure mode is
 * machine-checkable and can be mapped back to a UA_StatusCode at the open62541
 * boundary. toString() below is the single place that maps a status back to a
 * human-readable message, mirroring s7codec::CodecStatus.
 */
enum class OpcUaAdapterError {
    NULL_POINTER,
    DECODE_FAILED,
    ENCODE_FAILED,
    OUT_OF_RANGE,
    TYPE_MISMATCH,
    MEMBER_TYPE_MISMATCH,
    CODEC_ENTRY_NOT_FOUND,
    NO_EQUIVALENT_TYPE,
    VALUE_COPY_FAILED,
    ENUM_UNSUPPORTED_KIND,
    INVALID_ARRAY,
    INVALID_DTL,
    INVALID_DB_ENTRY,
    ARRAY_ALLOC_FAILED,
    BOOL_ARRAY_ALLOC_FAILED,
    BOOL_ARRAY_WRITE_FAILED,
    ALLOC_FAILED,
};

// Static translator: OpcUaAdapterError -> human-readable message.
inline const char* toString(OpcUaAdapterError error) {
    switch (error) {
        case OpcUaAdapterError::NULL_POINTER:
            return "null pointer received while decoding/encoding";
        case OpcUaAdapterError::DECODE_FAILED:
            return "failed to decode an S7 scalar from memory";
        case OpcUaAdapterError::ENCODE_FAILED:
            return "failed to encode a UA value into S7 memory";
        case OpcUaAdapterError::OUT_OF_RANGE:
            return "value out of range for the target S7 field";
        case OpcUaAdapterError::TYPE_MISMATCH:
            return "UA value type does not match the expected S7 type";
        case OpcUaAdapterError::MEMBER_TYPE_MISMATCH:
            return "UA struct member type does not match the looked-up type";
        case OpcUaAdapterError::CODEC_ENTRY_NOT_FOUND:
            return "no codec entry found for the target S7 type";
        case OpcUaAdapterError::NO_EQUIVALENT_TYPE:
            return "no equivalent OPC UA type for the decoded S7 value";
        case OpcUaAdapterError::VALUE_COPY_FAILED:
            return "failed to copy the UA value into the target member";
        case OpcUaAdapterError::ENUM_UNSUPPORTED_KIND:
            return "enum value does not carry an integer kind";
        case OpcUaAdapterError::INVALID_ARRAY:
            return "array is empty or of an unknown type";
        case OpcUaAdapterError::INVALID_DTL:
            return "DTL payload must be exactly 12 bytes";
        case OpcUaAdapterError::INVALID_DB_ENTRY:
            return "invalid data block entry or context";
        case OpcUaAdapterError::ARRAY_ALLOC_FAILED:
            return "failed to allocate the UA array";
        case OpcUaAdapterError::BOOL_ARRAY_ALLOC_FAILED:
            return "failed to allocate the UA boolean array";
        case OpcUaAdapterError::BOOL_ARRAY_WRITE_FAILED:
            return "failed to pack booleans into S7 memory";
        case OpcUaAdapterError::ALLOC_FAILED:
            return "memory allocation failed";
    }
    return "unknown OPC UA adapter error";
}

/**
 * @brief Map an adapter error to the UA_StatusCode reported to open62541
 *        callers. Used only at the functions that must return UA_STATUS.
 */
inline UA_StatusCode toUAStatusCode(OpcUaAdapterError error) {
    switch (error) {
        case OpcUaAdapterError::OUT_OF_RANGE:
            return UA_STATUSCODE_BADOUTOFRANGE;
        case OpcUaAdapterError::TYPE_MISMATCH:
        case OpcUaAdapterError::MEMBER_TYPE_MISMATCH:
        case OpcUaAdapterError::CODEC_ENTRY_NOT_FOUND:
        case OpcUaAdapterError::NO_EQUIVALENT_TYPE:
        case OpcUaAdapterError::ENUM_UNSUPPORTED_KIND:
            return UA_STATUSCODE_BADTYPEMISMATCH;
        case OpcUaAdapterError::DECODE_FAILED:
        case OpcUaAdapterError::INVALID_ARRAY:
        case OpcUaAdapterError::INVALID_DTL:
            return UA_STATUSCODE_BADDATAENCODINGINVALID;
        case OpcUaAdapterError::NULL_POINTER:
        case OpcUaAdapterError::ENCODE_FAILED:
        case OpcUaAdapterError::VALUE_COPY_FAILED:
        case OpcUaAdapterError::INVALID_DB_ENTRY:
        case OpcUaAdapterError::ARRAY_ALLOC_FAILED:
        case OpcUaAdapterError::BOOL_ARRAY_ALLOC_FAILED:
        case OpcUaAdapterError::BOOL_ARRAY_WRITE_FAILED:
        case OpcUaAdapterError::ALLOC_FAILED:
            return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_BADINTERNALERROR;
}

} // namespace sgrn::gateway::adapters