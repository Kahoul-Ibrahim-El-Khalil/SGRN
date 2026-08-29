#pragma once

#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/scl/types.hpp>

namespace sgrn::gateway::adapters::http
{

/**
 * @brief SclError outcomes that can flow out of the HTTP adapter's read/write
 *        handlers.
 *
 * Every fallible step inside the HTTP handlers returns this enum (via
 * sgrn::Result) so failures are machine-checkable and are translated to an
 * HTTP status code exactly once — at the wire boundary where the
 * httplib::Response is filled in. toString() is the single place mapping a
 * status back to a human-readable message, mirroring the OPC UA adapter's
 * errors.hpp pattern.
 */
enum class HttpAdapterError {
    NULL_POINTER,
    SCHEMA_NOT_FOUND,
    FIELD_NOT_FOUND,
    TYPE_MISMATCH,
    OUT_OF_RANGE,
    VALIDATION_FAILED,
    WRITE_FAILED,
    NOT_INITIALIZED,
    INTERNAL,
};

/// Static translator: HttpAdapterError -> human-readable message.
inline const char* toString(HttpAdapterError t_err) {
    switch (t_err) {
        case HttpAdapterError::NULL_POINTER:
            return "null pointer while handling request";
        case HttpAdapterError::SCHEMA_NOT_FOUND:
            return "no schema entry for the requested path";
        case HttpAdapterError::FIELD_NOT_FOUND:
            return "field not found in the twin";
        case HttpAdapterError::TYPE_MISMATCH:
            return "payload type does not match the field type";
        case HttpAdapterError::OUT_OF_RANGE:
            return "value or index out of range for the target field";
        case HttpAdapterError::VALIDATION_FAILED:
            return "request payload failed validation";
        case HttpAdapterError::WRITE_FAILED:
            return "failed to write value into PLC memory";
        case HttpAdapterError::NOT_INITIALIZED:
            return "twin state not attached yet";
        case HttpAdapterError::INTERNAL:
            return "internal HTTP adapter error";
    }
    return "unknown HTTP adapter error";
}

// -----------------------------------------------------------------------------
// Wire translators. Severity groupings intentionally mirror the OPC UA
// adapter's toUAStatusCode() in <sgrn/gateway/adapters/opcua/errors.hpp>:
//   - PLC_STATE_NOT_INITIALIZED is a connectivity-class error (503)
//   - DB_SEGMENT_NOT_FOUND / UNMAPPED_ARENA_REGION are "not found"-class (404)
//   - RANGE_* are out-of-range-class (400)
//   - NULL_BUFFER / INVALID_BIT_INDEX are internal-class (500)
// -----------------------------------------------------------------------------

/// PlcMemoryError -> HTTP status code.
constexpr int toHttpStatus(sgrn::gateway::twin::PlcMemoryError t_status) {
    using sgrn::gateway::twin::PlcMemoryError;
    switch (t_status) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
            return 503; // connectivity-class: twin not attached yet
        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
        case PlcMemoryError::UNMAPPED_ARENA_REGION:
            return 404; // "not found"-class
        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
            return 400; // out-of-range-class
        case PlcMemoryError::NULL_BUFFER:
        case PlcMemoryError::INVALID_BIT_INDEX:
            return 500; // internal-class
    }
    return 500;
}

/// scl::SclError (schema/field-level failure) -> HTTP status code.
constexpr int toHttpStatus(sgrn::scl::SclError t_err) {
    using sgrn::scl::SclError;
    switch (t_err) {
        case SclError::NotFound:
            return 404;
        case SclError::ParseError:
        case SclError::InvalidType:
        case SclError::Conflict:
            return 400;
        case SclError::OutOfRange:
            return 416;
        case SclError::SerializationError:
        case SclError::OptimizedAccess:
        case SclError::Generic:
        default:
            return 500;
    }
}

/// HttpAdapterError -> HTTP status code.
constexpr int toHttpStatus(HttpAdapterError t_err) {
    switch (t_err) {
        case HttpAdapterError::NULL_POINTER:
        case HttpAdapterError::INTERNAL:
            return 500;
        case HttpAdapterError::SCHEMA_NOT_FOUND:
        case HttpAdapterError::FIELD_NOT_FOUND:
            return 404;
        case HttpAdapterError::TYPE_MISMATCH:
        case HttpAdapterError::VALIDATION_FAILED:
        case HttpAdapterError::OUT_OF_RANGE:
            return 400;
        case HttpAdapterError::WRITE_FAILED:
            return 500;
        case HttpAdapterError::NOT_INITIALIZED:
            return 503;
    }
    return 500;
}

} // namespace sgrn::gateway::adapters::http
