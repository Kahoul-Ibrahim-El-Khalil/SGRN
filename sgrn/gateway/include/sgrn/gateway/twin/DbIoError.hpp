#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// DbIoError — dedicated error type for DbIOProvider.
//
// DbIOProvider sits at the intersection of three unrelated error domains and
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string_view>

#include <fmt/format.h>

#include <sgrn/gateway/twin/PlcMemory.hpp>    // PlcMemoryError
#include <sgrn/gateway/wrappers/s7/error.hpp> // S7Error
#include <sgrn/scl/errors.hpp>                // SclError

namespace sgrn::gateway::twin
{

enum class DbIoError : uint8_t {
    Success = 0,
    Generic = 1,
    FieldNotFound = 2,      ///< schema_.findField() missed — no such symbolic path
    EncodeFailed = 3,       ///< encodeFieldAt()/encodeScalar() rejected the value for this field's type
    NotConnected = 4,       ///< get()/put() called while the S7Client isn't connected
    NetworkReadFailed = 5,  ///< readMultiVars() failed, or the PLC rejected the item (Result != 0)
    NetworkWriteFailed = 6, ///< writeMultiVars() failed, or the PLC rejected the item (Result != 0)
    LocalMemoryFailed = 7,  ///< memory_.* (shadow arena / PlcMemoryError) failed
    SnapshotSyncFailed = 8, ///< pending_writes_ DbSnapshot::updateField() failed after a successful PLC write
};

constexpr std::string_view toString(DbIoError e) noexcept {
    switch (e) {
        case DbIoError::Success:
            return "success";
        case DbIoError::Generic:
            return "generic DB I/O error";
        case DbIoError::FieldNotFound:
            return "field not found";
        case DbIoError::EncodeFailed:
            return "value encode failed";
        case DbIoError::NotConnected:
            return "not connected";
        case DbIoError::NetworkReadFailed:
            return "network read failed";
        case DbIoError::NetworkWriteFailed:
            return "network write failed";
        case DbIoError::LocalMemoryFailed:
            return "local memory access failed";
        case DbIoError::SnapshotSyncFailed:
            return "snapshot sync failed";
    }
    return "unknown DB I/O error";
}

// ── Bridges from the three domains DbIOProvider touches ────────────────────

inline DbIoError fromS7ErrorToDbIoError(::sgrn::gateway::wrappers::s7::S7Error t_e) noexcept {
    using ::sgrn::gateway::wrappers::s7::S7Error;
    switch (t_e) {
        case S7Error::Success:
            return DbIoError::Success;
        case S7Error::NotConnected:
            return DbIoError::NotConnected;
        case S7Error::ReadError:
        case S7Error::Timeout:
        case S7Error::ConnectionFailed:
        case S7Error::PduError:
        case S7Error::DeviceBusy:
            return DbIoError::NetworkReadFailed;
        case S7Error::WriteError:
            return DbIoError::NetworkWriteFailed;
        case S7Error::InvalidParam:
            return DbIoError::EncodeFailed;
        case S7Error::Unknown:
            return DbIoError::Generic;
    }
    return DbIoError::Generic;
}

inline DbIoError fromPlcMemoryErrorToDbIoError(PlcMemoryError t_e) noexcept {
    (void)t_e; // every case maps to the same bucket today; kept as a real switch
               // (not a default-only function) so a new PlcMemoryError member
               // shows up here as a compiler warning, not a silent fallthrough.
    switch (t_e) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
        case PlcMemoryError::UNMAPPED_ARENA_REGION:
        case PlcMemoryError::NULL_BUFFER:
        case PlcMemoryError::INVALID_BIT_INDEX:
        case PlcMemoryError::UKNOWN:
        case PlcMemoryError::EXTERNAL:
            return DbIoError::LocalMemoryFailed;
    }
    return DbIoError::LocalMemoryFailed;
}

inline DbIoError fromSclErrorToDbIoError(::sgrn::scl::SclError t_e) noexcept {
    using ::sgrn::scl::SclError;
    switch (t_e) {
        case SclError::NotFound:
        case SclError::SymbolNotFound:
            return DbIoError::FieldNotFound;
        case SclError::InvalidType:
        case SclError::TypeMismatch:
        case SclError::OutOfRange:
        case SclError::ParseError:
        case SclError::InvalidJson:
        case SclError::InvalidFormat:
        case SclError::UnsupportedType:
        case SclError::CodecFailure:
        case SclError::BufferOverflow:
            return DbIoError::EncodeFailed;
        case SclError::FileNotFound:
        case SclError::IoError:
        case SclError::Conflict:
        case SclError::DuplicateDefinition:
        case SclError::SerializationError:
        case SclError::OptimizedAccess:
        case SclError::RecursionLimitExceeded:
        case SclError::Generic:
            return DbIoError::SnapshotSyncFailed;
    }
    return DbIoError::Generic;
}

} // namespace sgrn::gateway::twin

// fmt formatter so DbIoError can be used directly in fmt::format()/fmt::print(),
// same as SclError/ShellError.
template <>
struct fmt::formatter<::sgrn::gateway::twin::DbIoError> : formatter<std::string_view> {
    auto format(::sgrn::gateway::twin::DbIoError t_e, format_context& t_ctx) const {
        return formatter<std::string_view>::format(::sgrn::gateway::twin::toString(t_e), t_ctx);
    }
};

// ── Reverse bridge: DbIoError -> S7Error ────────────────────────────────────
// Lives here (rather than in wrappers/s7/error.hpp) for the same reason
// fromPlcMemoryErrorToS7Error lives in its own small adapter header instead of
// error.hpp: error.hpp is a light, widely-included header and shouldn't pull
// in twin/PlcMemory.hpp. DbIoError.hpp already depends on error.hpp (one
// direction only), so the reverse mapping belongs next to DbIoError itself.
//
// Used by ScriptDataBlock's setOpResult(Result<T, DbIoError>&) so the
// lastOpError()/lastOpOk() introspection surface — which is S7Error-typed —
// stays uniform regardless of which layer (S7 client, DbIOProvider, TagTable)
// produced the failure.
namespace sgrn::gateway::wrappers::s7
{

inline S7Error fromDbIoErrorToS7Error(::sgrn::gateway::twin::DbIoError t_e) noexcept {
    using ::sgrn::gateway::twin::DbIoError;
    switch (t_e) {
        case DbIoError::Success:
            return S7Error::Success;
        case DbIoError::NotConnected:
            return S7Error::NotConnected;
        case DbIoError::FieldNotFound:
        case DbIoError::EncodeFailed:
            return S7Error::InvalidParam;
        case DbIoError::NetworkReadFailed:
            return S7Error::ReadError;
        case DbIoError::NetworkWriteFailed:
            return S7Error::WriteError;
        case DbIoError::LocalMemoryFailed:
        case DbIoError::SnapshotSyncFailed:
        case DbIoError::Generic:
            return S7Error::Unknown;
    }
    return S7Error::Unknown;
}

} // namespace sgrn::gateway::wrappers::s7
