#pragma once

#include <fmt/format.h>

#include <string_view>

namespace sgrn::scl
{

/**
 * @brief SclError — single enum for all SCL / schema / IO failures.
 *
 * Mirrors gateway/adapters/opcua/errors.hpp OpcUaAdapterError pattern:
 * enum class + toString() overload, no payload struct.
 */
enum class SclError : uint16_t {
    Generic = 1,
    FileNotFound = 2,
    IoError = 3,
    ParseError = 4,
    Conflict = 5,
    NotFound = 6,
    OptimizedAccess = 8,
    InvalidType = 9,
    OutOfRange = 10,
    SerializationError = 11,
    InvalidJson = 12,
    SymbolNotFound = 13,
    DuplicateDefinition = 14,
    TypeMismatch = 15,
    UnsupportedType = 16,
    BufferOverflow = 17,
    RecursionLimitExceeded = 18,
    CodecFailure = 19,
    InvalidFormat = 20,
};

constexpr std::string_view toString(SclError e) noexcept {
    switch (e) {
        case SclError::Generic:
            return "generic error";
        case SclError::FileNotFound:
            return "file not found";
        case SclError::IoError:
            return "io error";
        case SclError::ParseError:
            return "parse error";
        case SclError::Conflict:
            return "conflict";
        case SclError::NotFound:
            return "not found";
        case SclError::OptimizedAccess:
            return "optimized access block cannot be decoded";
        case SclError::InvalidType:
            return "invalid type";
        case SclError::OutOfRange:
            return "out of range";
        case SclError::SerializationError:
            return "serialization error";
        case SclError::InvalidJson:
            return "invalid JSON";
        case SclError::SymbolNotFound:
            return "symbol not found";
        case SclError::DuplicateDefinition:
            return "duplicate definition";
        case SclError::TypeMismatch:
            return "type mismatch";
        case SclError::UnsupportedType:
            return "unsupported type";
        case SclError::BufferOverflow:
            return "buffer overflow";
        case SclError::RecursionLimitExceeded:
            return "recursion limit exceeded";
        case SclError::CodecFailure:
            return "codec failure";
        case SclError::InvalidFormat:
            return "invalid format";
    }
    return "unknown error";
}

} // namespace sgrn::scl

// fmt formatter so SclError can be used directly in fmt::format() / fmt::print().
// This is the single place that injects the enum's human-readable text into logs,
// so call sites do not need to sprinkle `.string()` / toString() everywhere.
template <>
struct fmt::formatter<::sgrn::scl::SclError> : formatter<std::string_view> {
    auto format(::sgrn::scl::SclError t_e, format_context& t_ctx) const {
        return formatter<std::string_view>::format(::sgrn::scl::toString(t_e), t_ctx);
    }
};
