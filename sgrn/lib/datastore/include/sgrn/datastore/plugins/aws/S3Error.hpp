#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// S3Error — typed error enum for S3Client / Garage operations.
//
// Mirrors the DbIoError pattern from sgrn/gateway/twin/DbIoError.hpp:
//   - enum class : uint16_t
//   - constexpr toString()  (no default: branch, -Wswitch catches missed cases)
//   - fmt::formatter specialisation
//   - fromHttpStatus() bridge from HTTP status / S3 XML error code
//   - toBackendError() bridge lives here (not in BackendError.hpp) so that
//     BackendError.hpp does not need to depend on S3 headers.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <sgrn/datastore/BackendError.hpp>

namespace sgrn::datastore::plugins::aws
{

enum class S3Error : uint16_t {
    NotInitialized,  ///< S3 client_ or threadpool is null at call time
    RequestFailed,   ///< The S3 HTTP call returned a non-success status
    NotFound,        ///< 404 / NoSuchKey / NoSuchBucket
    AccessDenied,    ///< 403 / AccessDenied
    Timeout,         ///< 408 / 504 / SlowDown / network timeout
    FilesystemError, ///< Local filesystem I/O error (open, read, write)
    Unknown,         ///< Catch-all for unrecognised errors
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

/// Map an HTTP status code (and optional S3 XML <Code>) to our typed S3Error.
inline S3Error fromHttpStatus(int status, std::string_view xml_code = "", bool is_timeout = false) noexcept {
    if (is_timeout || status == 408 || status == 504 || xml_code == "RequestTimeout" || xml_code == "SlowDown")
        return S3Error::Timeout;
    if (status == 404 || xml_code == "NoSuchKey" || xml_code == "NoSuchBucket" || xml_code == "NotFound")
        return S3Error::NotFound;
    if (status == 403 || xml_code == "AccessDenied" || xml_code == "SignatureDoesNotMatch")
        return S3Error::AccessDenied;
    if (status == 0)
        return S3Error::Unknown;
    return S3Error::RequestFailed;
}

/// Convert an S3Error (plus optional human-readable detail) into a BackendError
/// ready for service-layer propagation.
inline BackendError toBackendError(S3Error e, std::string_view detail = "") {
    std::string msg = detail.empty() ? std::string(toString(e)) : std::string(detail);
    return BackendError(BackendErrorKind::Garage, "Garage", std::move(msg));
}

} // namespace sgrn::datastore::plugins::aws

// ── fmt::formatter so S3Error can be used in SGRN_ERROR / fmt::format ────────
template <>
struct fmt::formatter<::sgrn::datastore::plugins::aws::S3Error> : formatter<std::string_view> {
    auto format(::sgrn::datastore::plugins::aws::S3Error e, format_context& ctx) const {
        return formatter<std::string_view>::format(::sgrn::datastore::plugins::aws::toString(e), ctx);
    }
};
