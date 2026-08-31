#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// RedisError — typed error enum for RedisMiddleware operations.
//
// Mirrors the DbIoError pattern:
//   - enum class : uint16_t
//   - constexpr toString()      (no default:, -Wswitch covers missed cases)
//   - fmt::formatter specialisation
//   - toBackendError() bridge lives here, not in BackendError.hpp
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string_view>

#include <fmt/format.h>

#include <sgrn/datastore/BackendError.hpp>

namespace sgrn::datastore::plugins
{

enum class RedisError : uint16_t {
    NotInitialized,   ///< drogon::app().getRedisClient() returned nullptr
    CommandFailed,    ///< A Redis command threw an exception
    NotFound,         ///< Key miss: result.isNil() on GET / GETEX
    ConnectionFailed, ///< Connection-level failure (distinct from command error)
    WrongType,        ///< Key holds a different data type than expected
    InternalError,    ///< Redis server internal error
    Timeout,          ///< Redis command timed out
    Unknown,          ///< Unclassified Redis error
};

constexpr std::string_view toString(RedisError e) noexcept {
    switch (e) {
        case RedisError::NotInitialized:
            return "Redis client not initialised";
        case RedisError::CommandFailed:
            return "Redis command failed";
        case RedisError::NotFound:
            return "Redis key not found";
        case RedisError::ConnectionFailed:
            return "Redis connection failed";
        case RedisError::WrongType:
            return "Redis key holds wrong data type";
        case RedisError::InternalError:
            return "Redis server internal error";
        case RedisError::Timeout:
            return "Redis command timed out";
        case RedisError::Unknown:
            return "unknown Redis error";
    }
    return "unknown Redis error";
}

/// Convert a RedisError into a BackendError ready for service-layer propagation.
inline ::sgrn::datastore::BackendError toBackendError(RedisError e, std::string_view detail = "") {
    std::string msg = detail.empty() ? std::string(toString(e)) : std::string(detail);
    return ::sgrn::datastore::BackendError(::sgrn::datastore::BackendErrorKind::Redis, "Redis", std::move(msg));
}

} // namespace sgrn::datastore::plugins

// ── fmt::formatter so RedisError can be used in SGRN_ERROR / fmt::format ─────
template <>
struct fmt::formatter<::sgrn::datastore::plugins::RedisError> : formatter<std::string_view> {
    auto format(::sgrn::datastore::plugins::RedisError e, format_context& ctx) const {
        return formatter<std::string_view>::format(::sgrn::datastore::plugins::toString(e), ctx);
    }
};
