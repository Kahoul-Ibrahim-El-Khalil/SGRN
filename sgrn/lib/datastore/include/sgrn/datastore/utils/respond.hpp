#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/ResultJson.hpp>
#include <sgrn/datastore/error/SgrnError.hpp>
#include <sgrn/datastore/error/exception.hpp>
#include <sgrn/types/HttpResponseCallback.hpp>
#include <cctype>
#include <json/json.h>
#include <string>
#include <string_view>
#include <trantor/utils/Date.h>
#include <type_traits>

namespace sgrn
{

constexpr const char json_content_type[] = "application/json";
constexpr const char plain_content_type[] = "text/plain";

// =========================================================
// 1. Core Response Factory
// =========================================================
inline drogon::HttpResponsePtr createEmptyResponse(drogon::HttpStatusCode t_http_code) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);
    return resp;
}

// =========================================================
// 2. Generic Text Response
// =========================================================
inline drogon::HttpResponsePtr createResponse(
    const std::string& t_body, drogon::HttpStatusCode t_http_code, std::string_view t_content_type = plain_content_type) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);
    resp->addHeader("Content-Type", std::string(t_content_type));
    resp->setBody(t_body);
    return resp;
}
inline drogon::HttpResponsePtr createResponse(
    std::string&& t_body, drogon::HttpStatusCode t_http_code, std::string_view t_content_type = plain_content_type) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);
    resp->addHeader("Content-Type", std::string(t_content_type));
    resp->setBody(std::move(t_body));
    return resp;
}

// =========================================================
// Helper: Map BackendErrorKind to HttpStatusCode
//
// This is the single, type-safe mapping point.  Every BackendError
// carries a kind_ that must route through here — no string comparisons.
// =========================================================
inline drogon::HttpStatusCode kindToHttpStatusCode(::sgrn::datastore::BackendErrorKind t_kind) noexcept {
    switch (t_kind) {
        case ::sgrn::datastore::BackendErrorKind::Database:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Redis:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Minio:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Postgrest:
            return drogon::k503ServiceUnavailable;
        case ::sgrn::datastore::BackendErrorKind::Auth:
            return drogon::k401Unauthorized;
        case ::sgrn::datastore::BackendErrorKind::Filesystem:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Network:
            return drogon::k503ServiceUnavailable;
        case ::sgrn::datastore::BackendErrorKind::Compression:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Hashing:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Runtime:
            return drogon::k500InternalServerError;
        case ::sgrn::datastore::BackendErrorKind::Generic:
            return drogon::k500InternalServerError;
    }
    return drogon::k500InternalServerError;
}

/// @deprecated Use kindToHttpStatusCode(err.kind_) instead.
/// Kept only to ease incremental migration of any remaining legacy call sites.
inline drogon::HttpStatusCode errorScopeToHttpStatusCode(const std::string& /*t_scope*/) {
    return drogon::k500InternalServerError;
}

// =========================================================
// 3. JSON Response Helpers
// =========================================================

// From Json::Value
inline drogon::HttpResponsePtr createJsonResponse(const Json::Value& t_json_object, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(t_json_object);
    resp->setStatusCode(t_http_code);
    return resp;
}

inline drogon::HttpResponsePtr createJsonResponse(Json::Value&& t_json_object, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(t_json_object));
    resp->setStatusCode(t_http_code);
    return resp;
}

// From ::sgrn::datastore::BackendError
inline drogon::HttpResponsePtr createJsonResponse(
    const ::sgrn::datastore::BackendError& t_error, std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    auto code = t_http_code.value_or(kindToHttpStatusCode(t_error.kind_));
    return createJsonResponse(::sgrn::utils::json::toJson(t_error), code);
}

// From ::sgrn::Result<T, BackendError>
template <typename T>
inline drogon::HttpResponsePtr createJsonResponse(
    const ::sgrn::Result<T, ::sgrn::datastore::BackendError>& t_result, std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    if (t_result.has_value()) {
        return createJsonResponse(::sgrn::utils::json::toJson(t_result), t_http_code.value_or(drogon::k200OK));
    } else {
        const auto& err = t_result.error();
        auto code = t_http_code.value_or(kindToHttpStatusCode(err.kind_));
        return createJsonResponse(::sgrn::utils::json::toJson(t_result), code);
    }
}

// From std::string
inline drogon::HttpResponsePtr createJsonResponse(const std::string& t_json, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);
    resp->addHeader("Content-Type", ::sgrn::json_content_type);
    resp->setBody(t_json);
    return resp;
}

inline drogon::HttpResponsePtr createJsonResponse(std::string&& t_json, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);
    resp->addHeader("Content-Type", ::sgrn::json_content_type);
    resp->setBody(std::move(t_json));
    return resp;
}

// From std::string_view
inline drogon::HttpResponsePtr createJsonResponse(std::string_view t_json, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    return createJsonResponse(std::string(t_json), t_http_code);
}

// From const char*
inline drogon::HttpResponsePtr createJsonResponse(const char* tp_json, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    return createJsonResponse(std::string_view(tp_json), t_http_code);
}

inline drogon::HttpResponsePtr createErrorResponse(
    std::string_view t_message, drogon::HttpStatusCode t_http_code, std::string_view t_scope = "Unknown") {

    // Auto-scope 401/403 errors to Authentication if otherwise unknown
    if (t_scope == "Unknown" && (t_http_code == drogon::k401Unauthorized || t_http_code == drogon::k403Forbidden)) {
        t_scope = "Authentication";
    }

    Json::Value error_json;
    error_json["error"] = std::string(t_message);
    error_json["scope"] = std::string(t_scope);
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(error_json));
    resp->setStatusCode(t_http_code);
    return resp;
}

// Overload for BackendError (pass by const ref — preferred)
inline drogon::HttpResponsePtr createErrorResponse(
    const ::sgrn::datastore::BackendError& t_error, std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    auto code = t_http_code.value_or(kindToHttpStatusCode(t_error.kind_));
    return createJsonResponse(::sgrn::utils::json::toJson(t_error), code);
}

// Overload for domain enums (ApiErrors)
// Produces the full wire shape: { error, scope, code, metadata? }
// The scope is derived from the code_name prefix (e.g. "ADMIN_API_USER_ALREADY_EXISTS" → "AdminApi").
template <typename EnumT, std::enable_if_t<std::is_enum_v<EnumT>, int> = 0>
inline drogon::HttpResponsePtr createErrorResponse(EnumT t_api_error, std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    auto err = makeHttpError(t_api_error);
    // Derive scope from code_name prefix: "ADMIN_API_X" → "AdminApi", "AUTH_API_X" → "AuthApi"
    std::string scope = "Unknown";
    {
        auto pos = err.code_name.find('_');
        if (pos != std::string::npos) {
            std::string first = err.code_name.substr(0, pos);
            auto pos2 = err.code_name.find('_', pos + 1);
            std::string second =
                (pos2 != std::string::npos) ? err.code_name.substr(pos + 1, pos2 - pos - 1) : err.code_name.substr(pos + 1);
            // Capitalize first letter, lowercase rest, concatenate
            if (!first.empty())
                first[0] = static_cast<char>(std::toupper(first[0]));
            for (size_t i = 1; i < first.size(); ++i)
                first[i] = static_cast<char>(std::tolower(first[i]));
            if (!second.empty())
                second[0] = static_cast<char>(std::toupper(second[0]));
            for (size_t i = 1; i < second.size(); ++i)
                second[i] = static_cast<char>(std::tolower(second[i]));
            scope = first + second;
        }
    }
    Json::Value error_json;
    error_json["error"] = err.message;
    error_json["scope"] = scope;
    auto code = t_http_code.value_or(err.status);
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(error_json));
    resp->setStatusCode(code);
    return resp;
}

// Overload for SgrnException
inline drogon::HttpResponsePtr createErrorResponse(const SgrnException& t_exception) {
    return createErrorResponse(t_exception.what(), t_exception.getStatus());
}

inline drogon::HttpResponsePtr createJsonErrorResponse(
    std::string_view t_message, drogon::HttpStatusCode t_http_code, std::string_view t_scope = "Unknown") {
    return createErrorResponse(t_message, t_http_code, t_scope);
}

// Overload for BackendError (pass by const ref — preferred)
inline drogon::HttpResponsePtr createJsonErrorResponse(
    const ::sgrn::datastore::BackendError& t_error, std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    return createErrorResponse(t_error, t_http_code);
}

// =========================================================
// 4. Senders
// =========================================================
inline void respondWithError(
    std::string_view t_message_view, drogon::HttpStatusCode t_http_code, const HttpResponseCallback& tsp_response_callback) {
    tsp_response_callback(createErrorResponse(t_message_view, t_http_code));
}

inline void respondWithJson(
    const Json::Value& t_json_object, drogon::HttpStatusCode t_http_code, const HttpResponseCallback& tsp_response_callback) {
    tsp_response_callback(createJsonResponse(t_json_object, t_http_code));
}

inline void respondWithJson(
    Json::Value&& t_json_object, drogon::HttpStatusCode t_http_code, const HttpResponseCallback& tsp_response_callback) {
    tsp_response_callback(createJsonResponse(std::move(t_json_object), t_http_code));
}

template <typename T>
inline void respondWithJson(const ::sgrn::Result<T>& t_result, const HttpResponseCallback& tsp_response_callback,
    std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    tsp_response_callback(createJsonResponse(t_result, t_http_code));
}

inline void respondWithText(
    std::string_view t_text, drogon::HttpStatusCode t_http_code, const HttpResponseCallback& tsp_response_callback) {
    tsp_response_callback(createResponse(std::string(t_text), t_http_code));
}

inline void sendResponse(std::string_view t_text, drogon::HttpStatusCode t_http_code, const HttpResponseCallback& tsp_response_callback) {
    respondWithText(t_text, t_http_code, tsp_response_callback);
}

} // namespace sgrn
