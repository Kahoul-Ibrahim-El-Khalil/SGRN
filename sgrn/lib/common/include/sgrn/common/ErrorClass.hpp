#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <stdexcept>

namespace sgrn::common
{

/// Protocol-agnostic severity class for a memory-access failure. Every
/// protocol adapter's wire-code translator (HTTP status, UA status code, CIP
/// general-status byte, Modbus exception code) maps from this class rather
/// than from any one backend's error enum — see sgrn/gateway's
/// adapters/{http,modbus,ethernetip,opcua}/errors.hpp. The twin-side
/// error-to-class mapping lives gateway-side next to the twin
/// (sgrn/gateway/common/ErrorClass.hpp) so this project stays independent
/// of sgrn_gateway_twin.
enum class ErrorClass : uint8_t {
    NotInitialized, // twin/connection not attached yet
    NotFound,       // segment/path does not exist
    OutOfRange,     // requested range/quantity too large
    Internal,       // null buffer, invalid bit index, etc.
};

constexpr std::string_view toString(ErrorClass t_class) noexcept {
    switch (t_class) {
        case ErrorClass::NotInitialized:
            return "memory not attached";
        case ErrorClass::NotFound:
            return "segment not found";
        case ErrorClass::OutOfRange:
            return "range exceeds allowed space";
        case ErrorClass::Internal:
            return "internal memory error";
    }
    return "unknown memory error";
}

} // namespace sgrn::common

template <>
struct fmt::formatter<sgrn::common::ErrorClass> : formatter<std::string_view> {
    // NOTE: delegates via format_to, not via formatter<string_view>::format —
    // the latter spelling breaks under extern fmt 12.2 (see sgrn issue log).
    auto format(sgrn::common::ErrorClass t_class, format_context& t_ctx) const {
        return fmt::format_to(t_ctx.out(), "{}", sgrn::common::toString(t_class));
    }
};
