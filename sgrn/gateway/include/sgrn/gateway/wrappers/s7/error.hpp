#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// S7Error — error type for Snap7 / S7 wire‑protocol failures.
//
// This type is intentionally separate from sgrn::scl::Error (schema/parse)
// and sgrn::scl::IoError (file‑system). It is the *only* error type that
// should appear on the return signatures of S7Client and S7ProtocolAdapter.
// ─────────────────────────────────────────────────────────────────────────────

#include <fmt/format.h>
#include <sgrn/Result.hpp>
#include <sgrn/scl/errors.hpp>
#include <snap7.h>
#include <string>
namespace sgrn::gateway::wrappers::s7
{

/**
 * @brief Error codes for Snap7 / S7 wire‑protocol failures.
 *
 * These are distinct from schema errors (sgrn::scl::SchemaCode) and
 * IO errors (sgrn::scl::IoCode). They represent failures in the actual
 * PLC communication layer only.
 */
enum class S7Error : uint8_t {
    Success = 0,
    ConnectionFailed = 1, ///< TCP/ISO connect rejected or timed out
    Timeout = 2,          ///< Operation timed out
    PduError = 3,         ///< Malformed or oversized PDU
    ReadError = 4,        ///< Area read operation rejected by PLC
    WriteError = 5,       ///< Area write operation rejected by PLC
    InvalidParam = 6,     ///< Bad parameter (null buffer, invalid range…)
    DeviceBusy = 7,       ///< PLC busy / resource unavailable
    NotConnected = 8,     ///< No active client connection
    Unknown = 99,         ///< Raw Snap7 code that doesn't map to the above
};

/**
 * @brief Convert S7Error to a human‑readable string.
 *
 * Inline to avoid ODR violations.
 */
inline const std::string_view toString(S7Error t_code) {
    switch (t_code) {
        case S7Error::Success:
            return "Success";
        case S7Error::ConnectionFailed:
            return "ConnectionFailed";
        case S7Error::Timeout:
            return "Timeout";
        case S7Error::PduError:
            return "PduError";
        case S7Error::ReadError:
            return "ReadError";
        case S7Error::WriteError:
            return "WriteError";
        case S7Error::InvalidParam:
            return "InvalidParam";
        case S7Error::DeviceBusy:
            return "DeviceBusy";
        case S7Error::NotConnected:
            return "NotConnected";
        case S7Error::Unknown:
            return "Unknown";
    }
    return "Unknown"; // fallback (should not happen)
}
// Snap7 error code families (from snap7.h / Snap7 docs):
//   0x00FFFFFF  — TCP layer errors
//   0x0DFFFFFF  — ISO layer errors
//   0xFFFFFFFx  — CLI errors (client, parameter, etc.)
//   0x00200000  — Tag / area errors

inline S7Error fromSnap7ErrorToS7Error(int t_err) noexcept {
    if (t_err == 0)
        return S7Error::Success;

    // ISO / TCP connection family
    if ((t_err & 0x000F0000) == 0x00020000 || (t_err & 0x000F0000) == 0x00030000)
        return S7Error::ConnectionFailed;

    // Time-out
    if ((t_err & 0x000000FF) == 0x02)
        return S7Error::Timeout;

    // PDU size / protocol framing
    if ((t_err & 0x0000FF00) == 0x0000D900 || (t_err & 0x0000FF00) == 0x0000DA00)
        return S7Error::PduError;

    // Client not connected / disconnected at ISO layer
    if (t_err == errIsoDisconnect)
        return S7Error::NotConnected;

    // Invalid parameter (null pointer, bad range, etc.)
    if (t_err == errCliInvalidParams || t_err == errCliFunctionRefused)
        return S7Error::InvalidParam;

    // Generic read / write failures (S7 function errors)
    if ((t_err & 0xFF000000) == 0x05000000)
        return S7Error::ReadError;
    if ((t_err & 0xFF000000) == 0x06000000)
        return S7Error::WriteError;

    return S7Error::Unknown;
}
inline S7Error fromSclErrorToS7Error(sgrn::scl::SclError t_e) noexcept {
    using sgrn::scl::SclError;
    S7Error code = S7Error::Unknown;
    switch (t_e) {
        case SclError::NotFound:
        case SclError::SymbolNotFound:
        case SclError::InvalidType:
        case SclError::OutOfRange:
        case SclError::ParseError:
        case SclError::InvalidJson:
        case SclError::InvalidFormat:
            code = S7Error::InvalidParam;
            break;
        case SclError::FileNotFound:
        case SclError::IoError:
            code = S7Error::ReadError;
            break;
        case SclError::Conflict:
        case SclError::DuplicateDefinition:
        case SclError::SerializationError:
        case SclError::CodecFailure:
        case SclError::OptimizedAccess:
        case SclError::TypeMismatch:
        case SclError::UnsupportedType:
        case SclError::BufferOverflow:
        case SclError::RecursionLimitExceeded:
        case SclError::Generic:
            code = S7Error::Unknown;
            break;
    }

    return S7Error{code};
}
} // namespace sgrn::gateway::wrappers::s7
