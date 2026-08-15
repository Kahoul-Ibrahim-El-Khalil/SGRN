#include <sgrn/datastore/handlers/auth.hpp>

#include <drogon/drogon.h>
#include <sgrn/datastore/plugins/redis/RedisMiddleware.hpp>
#include <sgrn/datastore/services/auth.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/utils/jsoncpp.hpp>

#ifdef DEBUG_AUTH_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("AuthHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("AuthHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("AuthHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("AuthHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::handlers::auth
{
using namespace drogon;

Task<HttpResponsePtr> AuthApiHandler::handleSignIn(HttpRequestPtr tsp_req) {
    const auto p_json = tsp_req->getJsonObject();
    if (p_json == nullptr) {
        co_return sgrn::createErrorResponse("Invalid JSON", k400BadRequest);
    }

    std::string email = p_json->get("email", "").asString();
    std::string password = p_json->get("password", "").asString();

    if (email.empty() || password.empty()) {
        co_return sgrn::createErrorResponse("Missing Email or Password", drogon::k400BadRequest);
    }

    co_return co_await performSignInProcess(tsp_req, std::move(email), std::move(password));
}

Task<HttpResponsePtr> AuthApiHandler::handleSignOut(HttpRequestPtr tsp_req) {
    auto token_opt = getSgrnToken(tsp_req);
    if (token_opt.has_value() == false) {
        // No token present, just redirect
        co_return HttpResponse::newRedirectionResponse("/");
    }

    try {
        std::string token = token_opt.value();

        auto redis_res = sgrn::datastore::core::getRedisMiddleware();
        if (redis_res.hasError()) {
            ERROR_LOG("Signout: RedisMiddleware not available for token: {}", token);
            co_return HttpResponse::newRedirectionResponse("/");
        }
        auto redis = redis_res.value();

        // 1. Get Session to find the email (to clean user cache)
        auto session_opt = co_await redis->getSession(token);
        if (session_opt.has_value() && session_opt.value().isMember("user") && session_opt.value()["user"].isMember("email")) {
            std::string email = session_opt.value()["user"]["email"].asString();
            co_await redis->deleteUserCache(email);

            auto db_res = sgrn::datastore::core::getDbClient();
            if (!db_res.hasError()) {
                auto db_client = db_res.value();
                if (session_opt.value().isMember("session_id") && session_opt.value()["session_id"].isInt64()) {
                    const int64_t session_id = session_opt.value()["session_id"].asInt64();
                    co_await db_client->execSqlCoro("UPDATE core.sessions SET terminated_at = NOW(), termination_reason = 'logout' "
                                                    "WHERE id = $1 AND terminated_at IS NULL",
                        session_id);
                }
            }
        } else {
            WARN_LOG("Signout: Session exists but missing user/email for token: {}", token);
        }

        // 2. Delete the Session Data (and upload key)
        co_await redis->deleteSession(token);

        // 3. Return success redirect
        co_return HttpResponse::newRedirectionResponse("/");

    } catch (const std::exception& e) {
        ERROR_LOG("Signout error: {}", e.what());
        // Still redirect even on error - best effort cleanup
        co_return HttpResponse::newRedirectionResponse("/");
    }
}

Task<HttpResponsePtr> AuthApiHandler::handleUpdatePassword(HttpRequestPtr tsp_req) {
    const auto p_json = tsp_req->getJsonObject();
    if (p_json == nullptr) {
        co_return sgrn::createErrorResponse("Invalid JSON", drogon::k400BadRequest);
    }

    std::string old_password = p_json->get("old_password", "").asString();
    std::string new_password = p_json->get("new_password", "").asString();

    if (old_password.empty() || new_password.empty()) {
        co_return sgrn::createErrorResponse("Missing old or new password", k400BadRequest);
    }

    // Extract email from request attributes (set by AuthFilter)
    std::string email = tsp_req->attributes()->get<Json::Value>("session_json")["user"]["email"].asString();

    co_return co_await sgrn::datastore::handlers::auth::updatePassword(
        tsp_req, std::move(email), std::move(old_password), std::move(new_password));
}

Task<HttpResponsePtr> AuthApiHandler::handleAutomatedServiceSignIn(drogon::HttpRequestPtr tsp_req) {
    const auto* p_body = tsp_req->getJsonObject().get();
    if (!p_body || !p_body->isMember("token") || !p_body->isMember("secret")) {
        co_return createJsonErrorResponse("Bad Request: 'token' and 'secret' fields are required", k400BadRequest);
    }

    const std::string token = (*p_body)["token"].asString();
    const std::string secret = (*p_body)["secret"].asString();

    if (token.empty() || secret.empty()) {
        co_return createJsonErrorResponse("Bad Request: 'token' and 'secret' must not be empty", k400BadRequest);
    }

    INFO_LOG("AuthApiHandler::handleAutomatedServiceSignIn — token prefix '{:.8}'", token);

    co_return co_await sgrn::datastore::handlers::auth::performAutomatedServiceSignInProcess(tsp_req, token, secret);
}

Task<HttpResponsePtr> AuthApiHandler::handleAutomatedServiceSignOut(HttpRequestPtr tsp_req) {
    // AutomatedServiceAuthFilter has already validated the session and populated session_json.
    const auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");

    INFO_LOG("AuthApiHandler::handleAutomatedServiceSignOut — automated_service_id={}", automated_service_id);

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db_client = db_res.value();

    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (token_opt.has_value() == false) {
        co_return createJsonErrorResponse("Unauthorized", k401Unauthorized);
    }
    const std::string& token = token_opt.value();

    try {
        // 1. Terminate the session row in the database.
        co_await db_client->execSqlCoro("UPDATE core.sessions "
                                        "SET    terminated_at      = NOW(), "
                                        "       termination_reason = 'logout' "
                                        "WHERE  automated_service_id = $1 "
                                        "  AND  token                = $2 "
                                        "  AND  terminated_at  IS NULL",
            automated_service_id, token);

        // 2. Evict the token from Redis
        auto redis_mw_res = sgrn::datastore::core::getRedisMiddleware();
        if (redis_mw_res.hasError() == false) {
            auto redis_client = redis_mw_res.value();
            co_await redis_client->del(token);
        }

        co_return createJsonResponse("Session terminated successfully", k200OK);

    } catch (const std::exception& ex) {
        ERROR_LOG("AuthApiHandler::handleAutomatedServiceSignOut error: {}", ex.what());
        co_return createJsonErrorResponse("Internal Server Error", k500InternalServerError);
    }
}

Task<HttpResponsePtr> AuthApiHandler::handleGetAutomatedServiceSession(HttpRequestPtr tsp_req) {
    // AutomatedServiceAuthFilter has already validated the session and extracted attributes.
    const auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");

    Json::Value response;
    response["automated_service_id"] = automated_service_id;

    // Forward the full automated service node back to the caller.
    if (session.isMember("user")) {
        response["automated_service"] = session["user"];
    }
    if (session.isMember("session_id")) {
        response["session_id"] = session["session_id"];
    }

    co_return HttpResponse::newHttpJsonResponse(std::move(response));
}
} // namespace sgrn::datastore::handlers::auth

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
