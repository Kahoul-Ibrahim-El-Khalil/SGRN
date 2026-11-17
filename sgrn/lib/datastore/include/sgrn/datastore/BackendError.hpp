#pragma once

#include <sgrn/Result.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace sgrn::datastore
{

// =========================================================
// TAXONOMY DECISION: Option (a) — Two intentionally separate layers
// =========================================================
// BackendErrorKind represents resource/infra-level failures
// (DB, S3, Auth, Filesystem, etc.).
// ApiErrors.hpp enums represent request/business-logic-level
// failures (validation, "already exists", "not found by business
// rule"). They are two separate layers that are never merged.
// Both must always produce the SAME wire shape:
//   { error, scope, code? }
// Plugin errors and API errors become the reflected BackendErrorResponse
// wire type at the HTTP boundary.
// =========================================================

enum class BackendErrorKind : uint8_t {
    Database,      ///< Postgres / Drogon ORM failures
    AlreadyExists, ///< Entity conflict / unique constraint violation
    Garage,        ///< S3 / Garage operation failures
    Auth,          ///< Authentication / authorisation logic (not an exception)
    Filesystem,    ///< Local file-system I/O errors
    Network,       ///< Generic network failures
    Compression,   ///< zstd / compression pipeline
    Hashing,       ///< SHA-512 / hash computation
    Runtime,       ///< Unclassified internal errors
    Generic,       ///< Fallback / unknown domain
};

constexpr std::string_view kindToScopeString(BackendErrorKind k) noexcept {
    switch (k) {
        case BackendErrorKind::Database:
            return "Database";
        case BackendErrorKind::AlreadyExists:
            return "Conflict";
        case BackendErrorKind::Garage:
            return "Garage";
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
//   - `scope` stays std::string: all scope literals are ≤ 15 chars and
//     therefore fit within GCC/Clang SSO — no heap alloc in practice.
//
// HTTP-transport note:
//   The old `status_override` / `setStatus()` are gone.  HTTP status codes
//   are derived exactly once, in kindToHttpStatusCode() (respond.hpp) or
//   toHttpError() (SgrnError.hpp), from `kind_`.  Service/plugin code must
//   never decide an HTTP status — that belongs at the handler boundary.
// ---------------------------------------------------------------------------

struct BackendError {
    BackendErrorKind kind;
    std::string scope;
    std::string message;
    std::optional<std::string> sub_code;

    // Default-constructs a Generic/Unknown error.
    BackendError()
        : kind(BackendErrorKind::Generic)
        , scope("Unknown") {
    }

    // Primary constructor: takes explicit kind + scope label + message.
    // The toBackendError() bridge functions in each plugin's *Error.hpp use
    // this form so that kind is always set correctly.
    BackendError(BackendErrorKind t_kind, std::string t_scope, std::string t_msg)
        : kind(t_kind)
        , scope(std::move(t_scope))
        , message(std::move(t_msg)) {
    }

    // Convenience: kind-only constructor — derives scope from kind.
    BackendError(BackendErrorKind t_kind, std::string t_msg)
        : kind(t_kind)
        , scope(kindToScopeString(t_kind))
        , message(std::move(t_msg)) {
    }

    // Chainable setters
    BackendError& setSubCode(std::string t_code) {
        sub_code = std::move(t_code);
        return *this;
    }
};

} // namespace sgrn::datastore
