#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// DbError — typed error enum for drogon::orm ORM call sites.
//
// Shared across the entire datastore (services/*.cpp, handlers/*.cpp,
// helpers/storage.cpp) — not owned by any single plugin.
//
// Mirrors the DbIoError pattern:
//   - enum class : uint16_t
//   - constexpr toString()          (no default:, -Wswitch covers missed cases)
//   - fromDrogonException() bridge  — classifies the ORM exception hierarchy
//   - toBackendError() bridge
//   - fmt::formatter specialisation
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string_view>

#include <drogon/orm/Exception.h>
#include <fmt/format.h>

#include <sgrn/datastore/BackendError.hpp>

namespace sgrn::datastore
{

enum class DbError : uint16_t {
    NotFound,              ///< UnexpectedRows with 0 rows — query expected ≥1 row
    ConstraintViolation,   ///< IntegrityConstraintViolation (unique / FK / check)
    ConnectionUnavailable, ///< Connection not available / pool exhausted
    QueryFailed,           ///< SqlError, DataException, or other statement-level failure
    Unknown,               ///< Any other DrogonDbException subclass
};

constexpr std::string_view toString(DbError e) noexcept {
    switch (e) {
        case DbError::NotFound:
            return "database record not found";
        case DbError::ConstraintViolation:
            return "database constraint violated";
        case DbError::ConnectionUnavailable:
            return "database connection unavailable";
        case DbError::QueryFailed:
            return "database query failed";
        case DbError::Unknown:
            return "unknown database error";
    }
    return "unknown database error";
}

/// Classify a DrogonDbException into the nearest DbError bucket.
/// This replaces the pattern of catching DrogonDbException and formatting
/// `e.base().what()` into a free-text BackendError by hand.
inline DbError fromDrogonException(const drogon::orm::DrogonDbException& e) noexcept {
    // drogon::orm::UnexpectedRows is a subclass of RangeError; the only case
    // where it is thrown is when CoroMapper gets 0 rows from a findOne() that
    // expects exactly one.  We expose that as NotFound.
    if (dynamic_cast<const drogon::orm::UnexpectedRows*>(&e.base()) != nullptr) {
        return DbError::NotFound;
    }
    // IntegrityConstraintViolation covers unique/FK/check failures.
    if (dynamic_cast<const drogon::orm::IntegrityConstraintViolation*>(&e.base()) != nullptr) {
        return DbError::ConstraintViolation;
    }
    // TimeoutError / InternalError / InsufficientResources → connectivity issues.
    if (dynamic_cast<const drogon::orm::TimeoutError*>(&e.base()) != nullptr) {
        return DbError::ConnectionUnavailable;
    }
    if (dynamic_cast<const drogon::orm::InternalError*>(&e.base()) != nullptr) {
        return DbError::ConnectionUnavailable;
    }
    // SqlError (and subclasses: DataException, SyntaxError, FeatureNotSupported,
    // InsufficientPrivilege, etc.) → the statement ran but produced an error.
    if (dynamic_cast<const drogon::orm::SqlError*>(&e.base()) != nullptr) {
        return DbError::QueryFailed;
    }
    return DbError::Unknown;
}

/// Convert a DbError (plus optional human-readable detail from e.base().what())
/// into a BackendError ready for service-layer propagation.
inline BackendError toBackendError(DbError e, std::string_view detail = "") {
    std::string msg = detail.empty() ? std::string(toString(e)) : std::string(detail);
    return BackendError(BackendErrorKind::Database, "Database", std::move(msg));
}

} // namespace sgrn::datastore

// ── fmt::formatter ────────────────────────────────────────────────────────────
template <>
struct fmt::formatter<::sgrn::datastore::DbError> : formatter<std::string_view> {
    auto format(::sgrn::datastore::DbError e, format_context& ctx) const {
        return formatter<std::string_view>::format(::sgrn::datastore::toString(e), ctx);
    }
};
