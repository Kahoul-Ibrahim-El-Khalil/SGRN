#pragma once

#include <fmt/format.h>
#include <sgrn/gateway/twin/DbIoError.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/errors.hpp>
#include <angelscript.h>
#include <string_view>

namespace sgrn::s7shell
{

enum class ShellError : uint16_t {
    Success = 0,
    Generic = 1,
    NotConnected = 2,
    ReadFailed = 3,
    WriteFailed = 4,
    SchemaError = 5,
    TypeMismatch = 6,
    NotFound = 7,
    InvalidParam = 8,
    Timeout = 9
};

inline const std::string_view toString(ShellError e) noexcept {
    switch (e) {
        case ShellError::Success:
            return "Success";
        case ShellError::Generic:
            return "Generic";
        case ShellError::NotConnected:
            return "NotConnected";
        case ShellError::ReadFailed:
            return "ReadFailed";
        case ShellError::WriteFailed:
            return "WriteFailed";
        case ShellError::SchemaError:
            return "SchemaError";
        case ShellError::TypeMismatch:
            return "TypeMismatch";
        case ShellError::NotFound:
            return "NotFound";
        case ShellError::InvalidParam:
            return "InvalidParam";
        case ShellError::Timeout:
            return "Timeout";
        default:
            return "Unknown";
    }
}

inline ShellError fromS7Error(sgrn::gateway::wrappers::s7::S7Error e) {
    using namespace sgrn::gateway::wrappers::s7;
    switch (e) {
        case S7Error::Success:
            return ShellError::Success;
        case S7Error::NotConnected:
            return ShellError::NotConnected;
        case S7Error::ReadError:
            return ShellError::ReadFailed;
        case S7Error::WriteError:
            return ShellError::WriteFailed;
        case S7Error::InvalidParam:
            return ShellError::InvalidParam;
        case S7Error::Timeout:
            return ShellError::Timeout;
        case S7Error::ConnectionFailed:
            return ShellError::NotConnected;
        case S7Error::DeviceBusy:
            return ShellError::Timeout;
        case S7Error::PduError:
            return ShellError::Generic;
        default:
            return ShellError::Generic;
    }
}

inline ShellError fromSclError(sgrn::scl::SclError e) {
    using namespace sgrn::scl;
    switch (e) {
        case SclError::NotFound:
            return ShellError::NotFound;
        case SclError::TypeMismatch:
            return ShellError::TypeMismatch;
        case SclError::InvalidType:
            return ShellError::TypeMismatch;
        case SclError::SerializationError:
            return ShellError::SchemaError;
        case SclError::ParseError:
            return ShellError::SchemaError;
        default:
            return ShellError::Generic;
    }
}

// DbIOProvider's own error domain — see sgrn/gateway/twin/DbIoError.hpp.
// Kept as an explicit switch (not a delegation through fromS7Error) so each
// DbIoError case maps to the ShellError a script author would actually want
// to see (e.g. FieldNotFound -> NotFound, not a generic read failure).
inline ShellError fromDbIoError(sgrn::gateway::twin::DbIoError e) {
    using namespace sgrn::gateway::twin;
    switch (e) {
        case DbIoError::Success:
            return ShellError::Success;
        case DbIoError::NotConnected:
            return ShellError::NotConnected;
        case DbIoError::FieldNotFound:
            return ShellError::NotFound;
        case DbIoError::EncodeFailed:
            return ShellError::TypeMismatch;
        case DbIoError::NetworkReadFailed:
            return ShellError::ReadFailed;
        case DbIoError::NetworkWriteFailed:
            return ShellError::WriteFailed;
        case DbIoError::LocalMemoryFailed:
            return ShellError::SchemaError;
        case DbIoError::SnapshotSyncFailed:
            return ShellError::SchemaError;
        default:
            return ShellError::Generic;
    }
}

inline void throwScriptException(const std::string& msg, ShellError code = ShellError::Generic) {
    if (auto* ctx = asGetActiveContext()) {
        std::string err = fmt::format("[{}] {}", toString(code), msg);
        ctx->SetException(err.c_str());
    }
}

} // namespace sgrn::s7shell
