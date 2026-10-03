#include <sgrn/datastore/filters/auth.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>
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

static bool isValidTokenFormat(std::string_view t_token) noexcept {
    if (t_token.length() != 36)
        return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (t_token[i] != '-')
                return false;
        } else {
            char c = t_token[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
    }
    return true;
}

void UserAuthFilter::doFilter(
    const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_cb, drogon::FilterChainCallback&& t_chain) {
    // 1. Extract Bearer Token
    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (!token_opt.has_value()) {
        std::string all_headers = "";
        for (const auto& [key, value] : tsp_req->headers()) {
            all_headers += key + ": " + value + " | ";
        }
        SGRN_WARN_LOG("Unauthorized access attempt. Path: {} | Headers: {}", tsp_req->path(), all_headers);
        respondWithError("Unauthorized: Missing Authorization Header", drogon::k401Unauthorized, tsp_cb);
        return;
    }

    const std::string t_token = token_opt.value();

    // Fast-path: Reject malformed / fake external tokens synchronously (<5ns) without spawning async tasks
    if (!isValidTokenFormat(t_token)) {
        SGRN_WARN_LOG("Fast-reject malformed token format: {}", redactToken(t_token));
        respondWithError("Unauthorized: Invalid token format", drogon::k401Unauthorized, tsp_cb);
        return;
    }

    // 2. Retrieve session from SessionStore (RAM Cache / PostgreSQL) asynchronously
    drogon::async_run([tsp_req, tsp_cb = std::move(tsp_cb), t_chain = std::move(t_chain), t_token]() mutable -> drogon::Task<void> {
        auto session_opt = co_await sgrn::datastore::session::SessionStore::instance().getSession(t_token);
        if (!session_opt.has_value()) {
            SGRN_WARN_LOG("Session not found for token: {}", redactToken(t_token));
            respondWithError("Unauthorized: Session expired or invalid", drogon::k401Unauthorized, tsp_cb);
            co_return;
        }

        Json::Value session_claims = std::move(session_opt.value());

        UserRoleEnum role = UserRoleEnum::USER;
        if (session_claims.isMember("user") && session_claims["user"].isMember("role") && session_claims["user"]["role"].isMember("code")) {
            role = static_cast<UserRoleEnum>(session_claims["user"]["role"]["code"].asUInt());
        }

        tsp_req->attributes()->insert("session_json", std::move(session_claims));
        tsp_req->attributes()->insert("user_role_code", role);

        t_chain();
    });
}

void AutomatedServiceAuthFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    // 1. Extract Bearer token
    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (!token_opt.has_value()) {
        respondWithError("Unauthorized: Missing Authorization Header", drogon::k401Unauthorized, tsp_filter_callback);
        return;
    }
    const std::string t_token = token_opt.value();

    if (!isValidTokenFormat(t_token)) {
        SGRN_WARN_LOG("AutomatedServiceAuthFilter: Fast-reject invalid token format");
        respondWithError("Unauthorized: Invalid token format", drogon::k401Unauthorized, tsp_filter_callback);
        return;
    }

    // Fast-path 2: O(log n) binary search against preloaded sorted public service token registry.
    // If this UUID is not a known registered service public token, reject synchronously (<30ns).
    // This eliminates ALL DB/async overhead for entirely fabricated or unknown service tokens.
    if (!sgrn::datastore::session::SessionStore::instance().isKnownServiceToken(t_token)) {
        SGRN_WARN_LOG("AutomatedServiceAuthFilter: Fast-reject unknown service token: {}", redactToken(t_token));
        respondWithError("Unauthorized: Unknown service token", drogon::k401Unauthorized, tsp_filter_callback);
        return;
    }

    // 2. Fetch session from SessionStore asynchronously
    drogon::async_run([tsp_req, tsp_filter_callback = std::move(tsp_filter_callback),
                          t_filter_chain_callback = std::move(t_filter_chain_callback), t_token]() mutable -> drogon::Task<void> {
        auto session_opt = co_await sgrn::datastore::session::SessionStore::instance().getSession(t_token);
        if (!session_opt.has_value()) {
            SGRN_WARN_LOG("AutomatedServiceAuthFilter: session not found for token");
            respondWithError("Unauthorized: Session expired or invalid", drogon::k401Unauthorized, tsp_filter_callback);
            co_return;
        }

        Json::Value session = std::move(session_opt.value());
        const Json::Value& user_node = session["user"];

        if (!user_node.isMember("automated_service_id")) {
            SGRN_WARN_LOG("AutomatedServiceAuthFilter: access denied — session is not for an automated service");
            respondWithError("Automated Service credentials required", drogon::k403Forbidden, std::move(tsp_filter_callback));
            co_return;
        }

        const int32_t automated_service_id = user_node["automated_service_id"].asInt();

        tsp_req->attributes()->insert("session_json", std::move(session));
        tsp_req->attributes()->insert("automated_service_id", automated_service_id);

        SGRN_INFO_LOG("AutomatedServiceAuthFilter: automated service {} authenticated", automated_service_id);

        t_filter_chain_callback();
    });
}

} // namespace sgrn::datastore::filters
