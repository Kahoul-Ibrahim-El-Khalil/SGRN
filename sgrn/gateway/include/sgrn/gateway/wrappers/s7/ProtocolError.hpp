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
enum class S7ProtocolCode : int {
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
 * @brief Convert S7ProtocolCode to a human‑readable string.
 *
 * Inline to avoid ODR violations.
 */
inline const std::string_view toString(S7ProtocolCode t_code) {
    switch (t_code) {
        case S7ProtocolCode::Success:
            return "Success";
        case S7ProtocolCode::ConnectionFailed:
            return "ConnectionFailed";
        case S7ProtocolCode::Timeout:
            return "Timeout";
        case S7ProtocolCode::PduError:
            return "PduError";
        case S7ProtocolCode::ReadError:
            return "ReadError";
        case S7ProtocolCode::WriteError:
            return "WriteError";
        case S7ProtocolCode::InvalidParam:
            return "InvalidParam";
        case S7ProtocolCode::DeviceBusy:
            return "DeviceBusy";
        case S7ProtocolCode::NotConnected:
            return "NotConnected";
        case S7ProtocolCode::Unknown:
            return "Unknown";
    }
    return "Unknown"; // fallback (should not happen)
}
// Snap7 error code families (from snap7.h / Snap7 docs):
//   0x00FFFFFF  — TCP layer errors
//   0x0DFFFFFF  — ISO layer errors
//   0xFFFFFFFx  — CLI errors (client, parameter, etc.)
//   0x00200000  — Tag / area errors

inline S7ProtocolCode classifySnap7(int t_err) noexcept {
    if (t_err == 0)
        return S7ProtocolCode::Success;

    // ISO / TCP connection family
    if ((t_err & 0x000F0000) == 0x00020000 || (t_err & 0x000F0000) == 0x00030000)
        return S7ProtocolCode::ConnectionFailed;

    // Time-out
    if ((t_err & 0x000000FF) == 0x02)
        return S7ProtocolCode::Timeout;

    // PDU size / protocol framing
    if ((t_err & 0x0000FF00) == 0x0000D900 || (t_err & 0x0000FF00) == 0x0000DA00)
        return S7ProtocolCode::PduError;

    // Client not connected / disconnected at ISO layer
    if (t_err == errIsoDisconnect)
        return S7ProtocolCode::NotConnected;

    // Invalid parameter (null pointer, bad range, etc.)
    if (t_err == errCliInvalidParams || t_err == errCliFunctionRefused)
        return S7ProtocolCode::InvalidParam;

    // Generic read / write failures (S7 function errors)
    if ((t_err & 0xFF000000) == 0x05000000)
        return S7ProtocolCode::ReadError;
    if ((t_err & 0xFF000000) == 0x06000000)
        return S7ProtocolCode::WriteError;

    return S7ProtocolCode::Unknown;
}
struct S7Error {
    S7ProtocolCode code_{S7ProtocolCode::Unknown};
    S7Error() = default;
    S7Error(S7ProtocolCode t_c)
        : code_(t_c) {
    }

    /**
     * @brief Construct from a raw Snap7 error integer.
     *
     * Maps well‑known Snap7 error families to S7ProtocolCode.
     * Accepts an optional override message; otherwise a default string is
     * derived from the Snap7 error text.
     */
    S7ProtocolCode code() const {
        return code_;
    }
    const std::string_view codeName() const {
        return toString(code_);
    }
    std::string string() const {
        return fmt::format("S7Error{{code={},  message=\"{}\"}}", static_cast<int>(code_), toString(code_));
    }
};

inline S7Error fromSnap7(int t_snap7_err, std::string t_msg) {
    const S7ProtocolCode code = classifySnap7(t_snap7_err);
    if (t_msg.empty()) {
        char buf[256]{};
        Cli_ErrorText(t_snap7_err, buf, sizeof(buf));
        t_msg = buf;
        if (t_msg.empty())
            t_msg = fmt::format("Snap7 error 0x{:08X}", static_cast<unsigned>(t_snap7_err));
    }
    return S7Error{code};
};
} // namespace sgrn::gateway::wrappers::s7

/**
 * @brief Structured error for S7 PLC wire‑protocol failures.
 *
 * Carries:
 *  - `code`        — a categorised enum code
 *  - `message`     — human‑readable description
 *  - `snap7_code`  — the raw int returned by Snap7 (preserved for diagnostics)
 *
 * Use `S7Error::fromSnap7(err)` to construct from a raw Snap7 return.
 */
