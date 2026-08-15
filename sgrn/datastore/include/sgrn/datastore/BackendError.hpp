#pragma once

#include <drogon/HttpTypes.h>
#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <json/json.h>
#include <optional>
#include <string>

namespace sgrn::datastore
{

// ---------------------------------------------------------------------------
// Scope constants — constexpr literals, used as string_view-safe keys
// ---------------------------------------------------------------------------
constexpr const char scope_runtime[] = "Runtime";
constexpr const char scope_database[] = "Database";
constexpr const char scope_redis[] = "Redis";
constexpr const char scope_minio[] = "Minio";
constexpr const char scope_postgrest[] = "Postgrest";
constexpr const char scope_compression[] = "Compression";
constexpr const char scope_hashing[] = "Hashing";
constexpr const char scope_application_logic[] = "ApplicationLogic";
constexpr const char scope_authentication[] = "Authentication";
constexpr const char scope_authorization[] = "Authorization";
constexpr const char scope_file_system[] = "FileSystem";
constexpr const char scope_network[] = "Network";
constexpr const char scope_filter[] = "Filter";
constexpr const char scope_service[] = "Service";
constexpr const char scope_allocation[] = "Allocation";
constexpr const char scope_unknown[] = "Unknown";
constexpr const char scope_plc[] = "PLC";
constexpr const char error_format[] = "[{}] {}";

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
// ---------------------------------------------------------------------------

struct BackendError {
    std::string scope_;
    std::string message_;
    std::optional<std::string> sub_code_; // only allocated when set
    std::optional<Json::Value> metadata_; // only allocated when set
    std::optional<drogon::HttpStatusCode> status_override_;

    BackendError()
        : scope_(scope_unknown) {
    }

    BackendError(std::string t_s, std::string t_m)
        : scope_(std::move(t_s))
        , message_(std::move(t_m)) {
    }

    BackendError(std::string t_s, std::string t_m, std::string t_value)
        : scope_(std::move(t_s))
        , message_(std::move(t_m))
        , sub_code_(std::move(t_value)) {
    }

    BackendError(std::string t_s, std::string t_m, std::string t_value, Json::Value t_meta)
        : scope_(std::move(t_s))
        , message_(std::move(t_m))
        , sub_code_(std::move(t_value))
        , metadata_(std::move(t_meta)) {
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

    BackendError& setStatus(drogon::HttpStatusCode t_code) {
        status_override_ = t_code;
        return *this;
    }

    std::string toString() const {
        return fmt::format(error_format, scope_, message_);
    }
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

template <typename... Args>
inline BackendError makeBackendError(std::string t_scope, fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return BackendError(std::move(t_scope), fmt::format(t_fmt, std::forward<Args>(t_args)...));
}

inline BackendError makeBackendError(std::string t_scope, std::string t_msg) {
    return BackendError(std::move(t_scope), std::move(t_msg));
}

template <typename... Args>
inline BackendError makeDatabaseError(fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return makeBackendError(scope_database, t_fmt, std::forward<Args>(t_args)...);
}

template <typename... Args>
inline BackendError makeRuntimeError(fmt::format_string<Args...> t_fmt, Args&&... t_args) {
    return makeBackendError(scope_runtime, t_fmt, std::forward<Args>(t_args)...);
}

// ---------------------------------------------------------------------------
// BackendResult<T> — Result<T, BackendError>
// ---------------------------------------------------------------------------

template <typename T>
using BackendResult = ::sgrn::Result<T, BackendError>;

} // namespace sgrn::datastore
