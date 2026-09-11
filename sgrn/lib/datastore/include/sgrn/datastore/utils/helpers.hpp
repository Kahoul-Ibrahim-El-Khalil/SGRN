/*sgrn/core/utils/helpers.hpp*/
#pragma once
#include <drogon/HttpTypes.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/types/HttpResponseCallback.hpp>
#include <sgrn/utils/jsoncpp.hpp>
#include <charconv> // ADDED: Required for std::from_chars
#include <string>
#include <string_view>
#include <thread>

namespace sgrn
{
using sgrn::datastore::BackendResult;
constexpr const char sgrn_session_token_field_identifier[] = "Authorization";

static std::string db_client_connection_error_message = "Failed to connect to DB client";
static std::string db_error = "Database error";

static std::string http_session_retrieval_error_message = "Failed to retrieve HTTP session";

inline drogon::SessionPtr getDrogonSession(const drogon::HttpRequestPtr& tsp_arg_req, HttpResponseCallback& tsp_arg_callback) {
    auto p_session = tsp_arg_req->getSession();
    if (!p_session) {
        respondWithError(http_session_retrieval_error_message, drogon::k500InternalServerError, tsp_arg_callback);
        return nullptr;
    }
    return p_session;
}

inline BackendResult<drogon::orm::DbClientPtr> getDrogonDbClient() {
    return sgrn::datastore::core::getDbClient();
}

inline void respondWithDbError(const drogon::orm::DrogonDbException& t_e, const HttpResponseCallback& tsp_arg_callback) {
    std::string err_msg = fmt::format("Database Error: {} - {}", db_error, t_e.base().what());
    respondWithError(std::move(err_msg), drogon::k500InternalServerError, tsp_arg_callback);
}

inline std::optional<std::string> getBearerToken(const drogon::HttpRequestPtr& tsp_http_req) {
    // 1. Try case-insensitive header lookup
    std::string auth_header = tsp_http_req->getHeader("Authorization");
    if (auth_header.empty()) {
        auth_header = tsp_http_req->getHeader("authorization");
    }

    if (auth_header.empty()) {
        return std::nullopt;
    }

    // 2. Check for "Bearer " prefix (case-insensitive)
    const std::string bearer_prefix = "Bearer ";
    if (auth_header.size() > bearer_prefix.size()) {
        // Case-insensitive check for "Bearer "
        bool match = true;
        for (size_t i = 0; i < bearer_prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(auth_header[i])) != std::tolower(static_cast<unsigned char>(bearer_prefix[i]))) {
                match = false;
                break;
            }
        }

        if (match) {
            // Trim whitespace from token
            std::string token = auth_header.substr(bearer_prefix.size());
            // Basic trim
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            if (token.empty())
                return std::nullopt;
            return token;
        }
    }

    return std::nullopt;
}

inline std::optional<std::string> getSgrnToken(const drogon::HttpRequestPtr& tsp_http_req) {
    return getBearerToken(tsp_http_req);
}

template <typename T>
inline std::optional<T> extractParameterNumericalValue(const drogon::HttpRequestPtr& tsp_http_req, const std::string& t_parameter_name) {
    static_assert(std::is_integral_v<T>, "T must be an integral type");

    const std::string value_str = tsp_http_req->getParameter(t_parameter_name);
    if (value_str.empty()) {
        return std::nullopt;
    }
    T value{};
    const char* p_begin = value_str.data();
    const char* p_end = value_str.data() + value_str.size();

    auto [ptr, ec] = std::from_chars(p_begin, p_end, value);

    // Parsing failed or extra characters exist
    if (ec != std::errc{} || ptr != p_end) {
        return std::nullopt;
    }

    return value;
}

template <typename T>
inline std::optional<T> extractHeaderNumericalValue(const drogon::HttpRequestPtr& tsp_req, const std::string& t_header) {
    static_assert(std::is_integral_v<T>, "T must be an integral type");

    // FIXED: Changed from getParameter to getHeader
    const std::string value_str = tsp_req->getHeader(t_header);
    if (value_str.empty()) {
        return std::nullopt;
    }
    T value{};
    const char* p_begin = value_str.data();
    const char* p_end = value_str.data() + value_str.size();

    auto [ptr, ec] = std::from_chars(p_begin, p_end, value);

    if (ec != std::errc{} || ptr != p_end) {
        return std::nullopt;
    }

    return value;
}

inline BackendResult<std::string> extractRequiredParam(const drogon::HttpRequestPtr& tsp_req, const std::string& t_name) {
    std::string value = tsp_req->getParameter(t_name);
    if (value.empty()) {
        return BackendResult<std::string>::Error("Application", "Missing required parameter: " + t_name);
    }
    return value;
}

inline std::optional<std::string> extractOptionalParam(const drogon::HttpRequestPtr& tsp_req, const std::string& t_name) {
    std::string value = tsp_req->getParameter(t_name);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

// ADDED: Security utility to prevent path traversal attacks
inline bool isValidStorageKey(std::string_view t_key) {
    // Reject path traversal attempts
    if (t_key.find("..") != std::string_view::npos)
        return false;
    if (t_key.find("//") != std::string_view::npos)
        return false;
    if (t_key.starts_with("/"))
        return false;
    if (t_key.empty())
        return false;
    return true;
}

} // namespace sgrn
