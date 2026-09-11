#pragma once

#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <json/json.h>
#include <optional>
#include <string>
#include <string_view>

namespace sgrn::datastore
{

// =========================================================
// TAXONOMY DECISION: Option (a) — Two intentionally separate layers
// =========================================================
// BackendErrorKind represents resource/infra-level failures
// (DB, Redis, S3, Postgrest, Auth, Filesystem, etc.).
// ApiErrors.hpp enums represent request/business-logic-level
// failures (validation, "already exists", "not found by business
// rule"). They are two separate layers that are never merged.
// Both must always produce the SAME wire shape:
//   { error, scope, code?, metadata? }
// The toBackendError() bridge functions in each plugin's
// *Error.hpp convert plugin enums to BackendError. The
// createErrorResponse(EnumT) bridge in respond.hpp converts
// ApiErrors enums to HttpError. Both paths converge on the same
// JSON wire format via ResultJson::toJson().
// =========================================================

enum class BackendErrorKind : uint8_t {
    Database,    ///< Postgres / Drogon ORM failures
    Redis,       ///< Redis operation failures
    Minio,       ///< S3 / MinIO operation failures
    Postgrest,   ///< PostgREST upstream failures
    Auth,        ///< Authentication / authorisation logic (not an exception)
    Filesystem,  ///< Local file-system I/O errors
    Network,     ///< Generic network failures
    Compression, ///< zstd / compression pipeline
    Hashing,     ///< SHA-512 / hash computation
    Runtime,     ///< Unclassified internal errors
    Generic,     ///< Fallback / unknown domain
};

constexpr std::string_view kindToScopeString(BackendErrorKind k) noexcept {
    switch (k) {
        case BackendErrorKind::Database:
            return "Database";
        case BackendErrorKind::Redis:
            return "Redis";
        case BackendErrorKind::Minio:
            return "Minio";
        case BackendErrorKind::Postgrest:
            return "Postgrest";
        case BackendErrorKind::Auth:
            return "Authentication";
        case BackendErrorKind::Filesystem:
            return "FileSystem";
        case BackendErrorKind::Network:
            return "Network";
        case BackendErrorKind::Compression:
            return "Compression";
        case BackendErrorKind::Hashing:
            return "Hashing";
        case BackendErrorKind::Runtime:
            return "Runtime";
        case BackendErrorKind::Generic:
            return "Unknown";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// BackendError — lean error object
//
// Performance notes vs. old version:
//   - `sub_code` is now std::optional<std::string>: no heap alloc on the
//     common path (99 % of errors don't carry a sub-code).
//   - `metadata` is now std::optional<Json::Value>: Json::Value default-ctor
//     is non-trivial and was previously always paid even when unused.
//   - `scope` stays std::string: all scope literals are ≤ 15 chars and
//     therefore fit within GCC/Clang SSO — no heap alloc in practice.
//
// HTTP-transport note:
//   The old `status_override_` / `setStatus()` are gone.  HTTP status codes
//   are derived exactly once, in kindToHttpStatusCode() (respond.hpp) or
//   toHttpError() (SgrnError.hpp), from `kind_`.  Service/plugin code must
//   never decide an HTTP status — that belongs at the handler boundary.
// ---------------------------------------------------------------------------

struct BackendError {
    BackendErrorKind kind_;
    std::string scope_;
    std::string message_;
    std::optional<std::string> sub_code_; // only allocated when set
    std::optional<Json::Value> metadata_; // only allocated when set

    // Default-constructs a Generic/Unknown error.
    BackendError()
        : kind_(BackendErrorKind::Generic)
        , scope_("Unknown") {
    }

    // Primary constructor: takes explicit kind + scope label + message.
    // The toBackendError() bridge functions in each plugin's *Error.hpp use
    // this form so that kind_ is always set correctly.
    BackendError(BackendErrorKind t_kind, std::string t_scope, std::string t_msg)
        : kind_(t_kind)
        , scope_(std::move(t_scope))
        , message_(std::move(t_msg)) {
    }

    // Convenience: kind-only constructor — derives scope from kind.
    BackendError(BackendErrorKind t_kind, std::string t_msg)
        : kind_(t_kind)
        , scope_(kindToScopeString(t_kind))
        , message_(std::move(t_msg)) {
    }

    // Legacy two-string constructor kept to ease the transition of call sites
    // that are not yet updated.  Marks kind as Generic so the HTTP mapping
    // stays visible until each site is updated.
    // NOTE: every remaining use of this constructor is a bug-to-fix target.
    BackendError(std::string t_s, std::string t_m)
        : kind_(BackendErrorKind::Generic)
        , scope_(std::move(t_s))
        , message_(std::move(t_m)) {
    }

    // Chainable setters
    BackendError& setSubCode(std::string t_code) {
        sub_code_ = std::move(t_code);
        return *this;
    }

    BackendError& setMetadata(Json::Value t_meta) {
        metadata_ = std::move(t_meta);
        return *this;
    }

    std::string toString() const {
        return fmt::format("[{}] {}", scope_, message_);
    }
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

/// Build a BackendError from an explicit kind (scope derived automatically).
template <typename... Args>
inline BackendError makeBackendError(BackendErrorKind t_kind, fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return BackendError(t_kind, fmt::format(t_fmt, std::forward<Args>(t_args)...));
}

inline BackendError makeBackendError(BackendErrorKind t_kind, std::string t_msg) {
    return BackendError(t_kind, std::move(t_msg));
}

/// Legacy overload — accepts a string scope key for call sites not yet updated.
/// DEPRECATED: prefer the BackendErrorKind overload above.
template <typename... Args>
inline BackendError makeBackendError(std::string t_scope, fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return BackendError(std::move(t_scope), fmt::format(t_fmt, std::forward<Args>(t_args)...));
}

inline BackendError makeBackendError(std::string t_scope, std::string t_msg) {
    return BackendError(std::move(t_scope), std::move(t_msg));
}

template <typename... Args>
inline BackendError makeDatabaseError(fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return makeBackendError(BackendErrorKind::Database, t_fmt, std::forward<Args>(t_args)...);
}

template <typename... Args>
inline BackendError makeRuntimeError(fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return makeBackendError(BackendErrorKind::Runtime, t_fmt, std::forward<Args>(t_args)...);
}

template <typename T>
using BackendResult = ::sgrn::Result<T, BackendError>;

} // namespace sgrn::datastore
