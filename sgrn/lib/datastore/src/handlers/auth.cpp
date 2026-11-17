#include <sgrn/datastore/handlers/auth.hpp>

#include <drogon/drogon.h>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/services/auth.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/wire/auth.hpp>

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

using namespace sgrn::datastore::auth;

namespace detail
{

struct AutomatedServiceSessionResponse {
    int32_t automated_service_id{0};
    std::optional<SessionUserInfo> automated_service;
    std::optional<int64_t> session_id;
};
} // namespace detail
using namespace detail;

Task<HttpResponsePtr> AuthApiHandler::handleSignIn(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<SignInRequest>(tsp_req);
    if (req_res.hasError() || req_res->email.empty() || req_res->password.empty()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{AuthApiError::InvalidPayload});
    }

    auto req = std::move(req_res.value());
    co_return co_await performSignInProcess(tsp_req, std::move(req.email), std::move(req.password));
}

Task<HttpResponsePtr> AuthApiHandler::handleSignOut(HttpRequestPtr tsp_req) {
    auto token_opt = getSgrnToken(tsp_req);
    if (!token_opt.has_value()) {
        co_return HttpResponse::newRedirectionResponse("/");
    }

    try {
        const std::string token = std::move(token_opt.value());
        // Revoke session: evicts from RAM cache, deletes from core.sessions, broadcasts NOTIFY
        co_await sgrn::datastore::session::SessionStore::instance().revokeSession(token);
        co_return HttpResponse::newRedirectionResponse("/");
    } catch (const std::exception& e) {
        ERROR_LOG("Signout error: {}", e.what());
        co_return HttpResponse::newRedirectionResponse("/");
    }
}

Task<HttpResponsePtr> AuthApiHandler::handleUpdatePassword(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<UpdatePasswordRequest>(tsp_req);
    if (req_res.hasError() || req_res->old_password.empty() || req_res->new_password.empty()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{AuthApiError::InvalidPayload});
    }

    auto req = std::move(req_res.value());

    // Extract email from request attributes (set by AuthFilter)
    std::string email;
    if (tsp_req->attributes()->find("auth_context")) {
        email = tsp_req->attributes()->get<filters::AuthContext>("auth_context").user.email;
    } else if (tsp_req->attributes()->find("session_claims")) {
        email = tsp_req->attributes()->get<SessionClaims>("session_claims").user.email;
    }

    co_return co_await sgrn::datastore::handlers::auth::updatePassword(
        tsp_req, std::move(email), std::move(req.old_password), std::move(req.new_password));
}

Task<HttpResponsePtr> AuthApiHandler::handleAutomatedServiceSignIn(drogon::HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<ServiceSignInRequest>(tsp_req);
    if (req_res.hasError() || req_res->token.empty() || req_res->secret.empty()) {
        co_return createTypedResponse(BackendErrorResponse{AuthApiError::InvalidPayload});
    }

    const auto& req = req_res.value();
    const std::string& token = req.token;
    const std::string& secret = req.secret;

    INFO_LOG("AuthApiHandler::handleAutomatedServiceSignIn — token prefix '{:.8}'", token);

    co_return co_await sgrn::datastore::handlers::auth::performAutomatedServiceSignInProcess(tsp_req, token, secret);
}

Task<HttpResponsePtr> AuthApiHandler::handleAutomatedServiceSignOut(HttpRequestPtr tsp_req) {
    // AutomatedServiceAuthFilter has already validated the typed session claims.
    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");

    INFO_LOG("AuthApiHandler::handleAutomatedServiceSignOut — automated_service_id={}", automated_service_id);

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }
    auto db_client = db_res.value();

    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (token_opt.has_value() == false) {
        co_return createTypedResponse(BackendErrorResponse{AuthApiError::MissingSessionToken});
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

        // 2. Evict the token from the in-process RAM cache
        sgrn::datastore::session::SessionStore::instance().evictLocal(token);

        co_return createTypedResponse(tsp_req, MessageResponse{"Session terminated successfully"}, k200OK);

    } catch (const std::exception& ex) {
        ERROR_LOG("AuthApiHandler::handleAutomatedServiceSignOut error: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<HttpResponsePtr> AuthApiHandler::handleGetAutomatedServiceSession(HttpRequestPtr tsp_req) {
    // AutomatedServiceAuthFilter has already validated the typed session claims.
    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");

    const auto& claims = tsp_req->getAttributes()->get<SessionClaims>("session_claims");
    std::optional<int64_t> session_id = claims.session_id != 0 ? std::optional<int64_t>(claims.session_id) : std::nullopt;
    co_return createTypedResponse(tsp_req, AutomatedServiceSessionResponse{automated_service_id, claims.user, session_id}, k200OK);
}
} // namespace sgrn::datastore::handlers::auth

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
