#include <sgrn/datastore/filters/auth.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/nosql/RedisClient.h>
#include <fmt/core.h>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <string>

namespace sgrn::datastore::filters
{

static std::string redactToken(const std::string& t_token) {
    if (t_token.length() < 16) {
        return std::string(t_token.length(), '*');
    }
    return t_token.substr(0, 8) + "..." + t_token.substr(t_token.length() - 8);
}

void UserAuthFilter::doFilter(
    const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_cb, drogon::FilterChainCallback&& t_chain) {
    // 1. Extract Bearer Token
    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (token_opt.has_value() == false) {
        std::string all_headers = "";
        for (const auto& [key, value] : tsp_req->headers()) {
            all_headers += key + ": " + value + " | ";
        }
        SGRN_WARN_LOG("Unauthorized access attempt. Path: {} | Headers: {}", tsp_req->path(), all_headers);
        respondWithError("Unauthorized: Missing Authorization Header", drogon::k401Unauthorized, tsp_cb);
        return;
    }

    const std::string& t_token = token_opt.value();

    // 4. Retrieve session data from Redis
    auto redis_res = sgrn::datastore::core::getRedisMiddleware();
    if (redis_res.hasError()) {
        SGRN_ERROR_LOG("RedisMiddleware not available for session lookup");
        respondWithError("Internal Auth Error", drogon::k503ServiceUnavailable, tsp_cb);
        return;
    }
    auto redis = redis_res.value();

    auto session_claims_res = redis->getSessionSync(t_token);
    if (session_claims_res.hasError()) {
        if (session_claims_res.error().message_ == "key not found") {
            SGRN_WARN_LOG("Session not found in Redis for token: {}", redactToken(t_token));
            respondWithError("Unauthorized: Session expired or invalid", drogon::k401Unauthorized, tsp_cb);
        } else {
            SGRN_ERROR_LOG("Redis error for session lookup: {}", session_claims_res.error().toString());
            respondWithError("Internal Auth Error", drogon::k503ServiceUnavailable, tsp_cb);
        }
        return;
    }
    Json::Value session_claims = std::move(session_claims_res.value());

    // 6. Populate Request Attributes for Handlers
    // Extract role BEFORE the move — once session_claims is moved the object is empty.
    UserRoleEnum role = UserRoleEnum::USER;
    if (session_claims.isMember("user") && session_claims["user"].isMember("role") && session_claims["user"]["role"].isMember("code")) {
        role = static_cast<UserRoleEnum>(session_claims["user"]["role"]["code"].asUInt());
    }

    // Move the full payload into the request for handlers to consume.
    tsp_req->attributes()->insert("session_json", std::move(session_claims));
    tsp_req->attributes()->insert("user_role_code", role);

    t_chain();
}

void AutomatedServiceAuthFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    // ------------------------------------------------------------------
    // 1. Extract Bearer token
    // ------------------------------------------------------------------
    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (token_opt.has_value() == false) {
        respondWithError("Unauthorized: Missing Authorization Header", drogon::k401Unauthorized, tsp_filter_callback);
        return;
    }
    const std::string& t_token = token_opt.value();

    // ------------------------------------------------------------------
    // 2. Fetch session from Redis
    // ------------------------------------------------------------------
    auto redis_res = sgrn::datastore::core::getRedisMiddleware();
    if (redis_res.hasError()) {
        SGRN_ERROR_LOG("AutomatedServiceAuthFilter: RedisMiddleware unavailable");
        respondWithError("Internal Auth Error", drogon::k503ServiceUnavailable, tsp_filter_callback);
        return;
    }
    auto redis = redis_res.value();

    auto session_res = redis->getSessionSync(t_token);
    if (session_res.hasError()) {
        if (session_res.error().message_ == "key not found") {
            SGRN_WARN_LOG("AutomatedServiceAuthFilter: session not found for token");
            respondWithError("Unauthorized: Session expired or invalid", drogon::k401Unauthorized, tsp_filter_callback);
        } else {
            SGRN_ERROR_LOG("AutomatedServiceAuthFilter Redis error: {}", session_res.error().toString());
            respondWithError("Internal Auth Error", drogon::k503ServiceUnavailable, tsp_filter_callback);
        }
        return;
    }
    Json::Value session = std::move(session_res.value());

    // ------------------------------------------------------------------
    // 3. Assert this session belongs to an automated service
    // ------------------------------------------------------------------
    const Json::Value& user_node = session["user"];

    // We check for the explicit presence of "automated_service_id" in the session payload
    if (!user_node.isMember("automated_service_id")) {
        SGRN_WARN_LOG("AutomatedServiceAuthFilter: access denied — session is not for an automated service");
        respondWithError("Automated Service credentials required", drogon::k403Forbidden, std::move(tsp_filter_callback));
        return;
    }

    // ------------------------------------------------------------------
    // 4. Inject attributes for downstream handlers
    // ------------------------------------------------------------------
    const int32_t automated_service_id = user_node["automated_service_id"].asInt();

    tsp_req->attributes()->insert("session_json", std::move(session));
    tsp_req->attributes()->insert("automated_service_id", automated_service_id);

    SGRN_INFO_LOG("AutomatedServiceAuthFilter: automated service {} authenticated", automated_service_id);

    t_filter_chain_callback();
}

} // namespace sgrn::datastore::filters
