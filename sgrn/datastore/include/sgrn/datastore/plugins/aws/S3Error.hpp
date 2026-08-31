#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// S3Error — typed error enum for S3Client / MinIO operations.
//
// Mirrors the DbIoError pattern from sgrn/gateway/twin/DbIoError.hpp:
//   - enum class : uint16_t
//   - constexpr toString()  (no default: branch, -Wswitch catches missed cases)
//   - fmt::formatter specialisation
//   - fromAwsError() bridge from the SDK's own typed error code
//   - toBackendError() bridge lives here (not in BackendError.hpp) so that
//     BackendError.hpp does not need to depend on AWS SDK headers.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string_view>

#include <fmt/format.h>
#include <aws/core/client/AWSError.h>
#include <aws/s3/S3Errors.h>

#include <sgrn/datastore/BackendError.hpp>

namespace sgrn::datastore::plugins::aws
{

enum class S3Error : uint16_t {
    NotInitialized,  ///< S3 client_ or threadpool is null at call time
    RequestFailed,   ///< The AWS SDK call returned a non-success outcome
    NotFound,        ///< NO_SUCH_KEY or NO_SUCH_BUCKET
    AccessDenied,    ///< ACCESS_DENIED from the SDK
    Timeout,         ///< Network timeout
    FilesystemError, ///< Local filesystem I/O error (open, read, write)
    Unknown,         ///< Catch-all for unrecognised SDK errors
};

constexpr std::string_view toString(S3Error e) noexcept {
    switch (e) {
        case S3Error::NotInitialized:
            return "S3 client not initialised";
        case S3Error::RequestFailed:
            return "S3 request failed";
        case S3Error::NotFound:
            return "S3 object/bucket not found";
        case S3Error::AccessDenied:
            return "S3 access denied";
        case S3Error::Timeout:
            return "S3 request timed out";
        case S3Error::FilesystemError:
            return "Local filesystem I/O error";
        case S3Error::Unknown:
            return "unknown S3 error";
    }
    return "unknown S3 error";
}

/// Map the SDK's own typed error code to our enum.
/// This replaces the pattern of catching std::exception and discarding the
/// SDK-level S3Errors value; every !outcome.IsSuccess() path should call this.
inline S3Error fromAwsError(const Aws::Client::AWSError<Aws::S3::S3Errors>& e) noexcept {
    switch (e.GetErrorType()) {
        case Aws::S3::S3Errors::NO_SUCH_KEY:
        case Aws::S3::S3Errors::NO_SUCH_BUCKET:
        case Aws::S3::S3Errors::RESOURCE_NOT_FOUND:
            return S3Error::NotFound;
        case Aws::S3::S3Errors::ACCESS_DENIED:
            return S3Error::AccessDenied;
        case Aws::S3::S3Errors::REQUEST_TIMEOUT:
        case Aws::S3::S3Errors::SLOW_DOWN: // also a transient / throttle case
            return S3Error::Timeout;
        case Aws::S3::S3Errors::UNKNOWN:
            return S3Error::Unknown;
        default:
            return S3Error::RequestFailed;
    }
}

/// Convert an S3Error (plus optional human-readable detail) into a BackendError
/// ready for service-layer propagation.  Lives here — not in BackendError.hpp —
/// so that BackendError.hpp remains free of AWS SDK headers.
inline ::sgrn::datastore::BackendError toBackendError(S3Error e, std::string_view detail = "") {
    std::string msg = detail.empty() ? std::string(toString(e)) : std::string(detail);
    return ::sgrn::datastore::BackendError(::sgrn::datastore::BackendErrorKind::Minio, "Minio", std::move(msg));
}

} // namespace sgrn::datastore::plugins::aws

// ── fmt::formatter so S3Error can be used in SGRN_ERROR / fmt::format ────────
template <>
struct fmt::formatter<::sgrn::datastore::plugins::aws::S3Error> : formatter<std::string_view> {
    auto format(::sgrn::datastore::plugins::aws::S3Error e, format_context& ctx) const {
        return formatter<std::string_view>::format(::sgrn::datastore::plugins::aws::toString(e), ctx);
    }
};
