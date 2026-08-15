#include <regex>
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
// Helper: Map error scope string to HttpStatusCode
// =========================================================
inline drogon::HttpStatusCode errorScopeToHttpStatusCode(const std::string& t_scope) {
    if (t_scope == ::sgrn::datastore::scope_runtime) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_database) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_redis) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_minio) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_postgrest) {
        return drogon::k503ServiceUnavailable;
    } else if (t_scope == ::sgrn::datastore::scope_compression) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_hashing) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_application_logic) {
        return drogon::k400BadRequest;
    } else if (t_scope == ::sgrn::datastore::scope_authentication) {
        return drogon::k401Unauthorized;
    } else if (t_scope == ::sgrn::datastore::scope_authorization) {
        return drogon::k403Forbidden;
    } else if (t_scope == ::sgrn::datastore::scope_file_system) {
        return drogon::k500InternalServerError;
    } else if (t_scope == ::sgrn::datastore::scope_network) {
        return drogon::k503ServiceUnavailable;
    } else {
        return drogon::k500InternalServerError;
    }
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
    auto code = t_http_code.value_or(t_error.status_override_.value_or(errorScopeToHttpStatusCode(t_error.scope_)));
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
        auto code = t_http_code.value_or(err.status_override_.value_or(errorScopeToHttpStatusCode(err.scope_)));
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
    auto code = t_http_code.value_or(t_error.status_override_.value_or(::sgrn::errorScopeToHttpStatusCode(t_error.scope_)));
    return createJsonResponse(::sgrn::utils::json::toJson(t_error), code);
}

// Overload for domain enums (ApiErrors)
template <typename EnumT, std::enable_if_t<std::is_enum_v<EnumT>, int> = 0>
inline drogon::HttpResponsePtr createErrorResponse(EnumT t_api_error, std::optional<drogon::HttpStatusCode> t_http_code = std::nullopt) {
    auto err = makeHttpError(t_api_error);
    return createErrorResponse(err.message, t_http_code.value_or(err.status));
}

// Overload for SgrnException
inline drogon::HttpResponsePtr createErrorResponse(const SgrnException& t_exception) {
    return createErrorResponse(t_exception.what(), t_exception.getStatus());
}

inline drogon::HttpResponsePtr createJsonErrorResponse(
    std::string_view t_message, drogon::HttpStatusCode t_http_code, std::string_view t_scope = "Unknown") {
    return createErrorResponse(t_message, t_http_code, t_scope);
}

// Overload for ::sgrn::datastore::BackendError
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
