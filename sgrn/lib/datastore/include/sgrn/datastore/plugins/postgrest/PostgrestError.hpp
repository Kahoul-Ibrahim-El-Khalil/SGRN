#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// PostgrestError — typed error enum for PostgrestClient operations.
//
// Mirrors the DbIoError pattern:
//   - enum class : uint16_t
//   - constexpr toString()      (no default:, -Wswitch covers missed cases)
//   - fmt::formatter specialisation
//   - toBackendError() bridge lives here, not in BackendError.hpp
//
// Note on UpstreamHttpError: the upstream HTTP status code varies per call and
// is carried by the response object, not encoded into the enum.  Callers that
// need to forward the upstream status to the client should inspect the response
// directly (see proxy.cpp).  The enum only signals *that* a non-2xx response
// was received.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string_view>

#include <fmt/format.h>

#include <sgrn/datastore/BackendError.hpp>

namespace sgrn::datastore::plugins
{

enum class PostgrestError : uint16_t {
    NotInitialized,    ///< PostgrestClient plugin (client_ ptr) is null
    RequestFailed,     ///< Exception thrown while sending to PostgREST
    UpstreamHttpError, ///< PostgREST returned a non-2xx HTTP status (status in response, not here)
    Timeout,           ///< Network timeout reaching PostgREST
    Unknown,           ///< Unclassified PostgREST error
};

constexpr std::string_view toString(PostgrestError e) noexcept {
    switch (e) {
        case PostgrestError::NotInitialized:
            return "Postgrest client not initialised";
        case PostgrestError::RequestFailed:
            return "Postgrest request failed";
        case PostgrestError::UpstreamHttpError:
            return "Postgrest upstream returned non-2xx status";
        case PostgrestError::Timeout:
            return "Postgrest request timed out";
        case PostgrestError::Unknown:
            return "unknown Postgrest error";
    }
    return "unknown Postgrest error";
}

/// Convert a PostgrestError into a BackendError ready for service-layer propagation.
inline ::sgrn::datastore::BackendError toBackendError(PostgrestError e, std::string_view detail = "") {
    std::string msg = detail.empty() ? std::string(toString(e)) : std::string(detail);
    return ::sgrn::datastore::BackendError(::sgrn::datastore::BackendErrorKind::Postgrest, "Postgrest", std::move(msg));
}

} // namespace sgrn::datastore::plugins

// ── fmt::formatter ────────────────────────────────────────────────────────────
template <>
struct fmt::formatter<::sgrn::datastore::plugins::PostgrestError> : formatter<std::string_view> {
    auto format(::sgrn::datastore::plugins::PostgrestError e, format_context& ctx) const {
        return formatter<std::string_view>::format(::sgrn::datastore::plugins::toString(e), ctx);
    }
};
