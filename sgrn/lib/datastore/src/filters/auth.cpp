#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/filters/auth.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>
#include <fmt/core.h>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
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
        tsp_cb(createTypedResponse(BackendErrorResponse{"Unauthorized: Missing Authorization Header", drogon::k401Unauthorized}));
        return;
    }

    const std::string t_token = token_opt.value();

    // Fast-path: Reject malformed / fake external tokens synchronously (<5ns) without spawning async tasks
    if (!isValidTokenFormat(t_token)) {
        tsp_cb(createTypedResponse(BackendErrorResponse{"Unauthorized: Invalid token format", drogon::k401Unauthorized}));
        return;
    }

    // 2. Retrieve session from SessionStore (RAM Cache / PostgreSQL) asynchronously
    drogon::async_run([tsp_req, tsp_cb = std::move(tsp_cb), t_chain = std::move(t_chain), t_token]() mutable -> drogon::Task<void> {
        std::optional<datastore::auth::SessionClaims> session_opt =
            co_await sgrn::datastore::session::SessionStore::instance().getSession(t_token);

        if (!session_opt.has_value()) {
            tsp_cb(createTypedResponse(BackendErrorResponse{"Unauthorized: Session expired or invalid", drogon::k401Unauthorized}));
            co_return;
        }

        const auto& claims = session_opt.value();

        UserRoleEnum role = static_cast<UserRoleEnum>(claims.user.role.code);

        // Hydrate admin level from cache (fail closed to NONE)
        AdminLevel level = AdminLevel::NONE;
        int32_t uid = claims.user.id;
        std::string org = claims.user.organisation;
        std::string email = claims.user.email;
        if (uid > 0) {
            level = co_await sgrn::datastore::filters::AdminLevelCache::instance().resolveAdminLevel(uid, org, std::nullopt, email);
        }

        // Determine role name from claims
        std::string role_name = "user";
        if (!claims.user.role.name.empty()) {
            role_name = claims.user.role.name;
        }

        AuthContext auth_ctx;
        auth_ctx.user.id = uid;
        auth_ctx.user.email = std::move(email);
        auth_ctx.user.organisation = std::move(org);
        auth_ctx.user.role = role_name;
        auth_ctx.user.admin_level = level;
        auth_ctx.user.admin_level_name = adminLevelToString(level);
        auth_ctx.user.is_platform_admin = (level == AdminLevel::PLATFORM);
        if (claims.session_id != 0) {
            auth_ctx.session_id = claims.session_id;
        }

        tsp_req->attributes()->insert("tenant_id", auth_ctx.user.organisation);
        tsp_req->attributes()->insert("auth_context", std::move(auth_ctx));
        tsp_req->attributes()->insert("session_claims", claims);
        tsp_req->attributes()->insert("admin_level", level);
        tsp_req->attributes()->insert("caller_admin_level", level);
        tsp_req->attributes()->insert("is_platform_admin", level == AdminLevel::PLATFORM);
        tsp_req->attributes()->insert("user_role_code", role);

        t_chain();
    });
}

void AutomatedServiceAuthFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    // 1. Extract Bearer token
    std::optional<std::string> token_opt = getBearerToken(tsp_req);
    if (!token_opt.has_value()) {
        tsp_filter_callback(
            createTypedResponse(BackendErrorResponse{"Unauthorized: Missing Authorization Header", drogon::k401Unauthorized}));
        return;
    }
    const std::string t_token = token_opt.value();

    if (!isValidTokenFormat(t_token)) {
        SGRN_WARN_LOG("AutomatedServiceAuthFilter: Fast-reject invalid token format");
        tsp_filter_callback(createTypedResponse(BackendErrorResponse{"Unauthorized: Invalid token format", drogon::k401Unauthorized}));
        return;
    }

    // Fast-path 2: reject fabricated tokens synchronously against preloaded in-RAM
    // registries. A legitimate caller presents either (a) an automated service
    // *public* token (O(log n) binary search) or (b) a *session* token issued by
    // sign-in (O(1) unordered_set lookup — sign-in returns fresh session UUIDs that
    // will never appear in the public-token registry). Tokens in neither registry
    // are entirely fabricated — reject with zero DB/async overhead.
    if (!sgrn::datastore::session::SessionStore::instance().isKnownServiceToken(t_token) &&
        !sgrn::datastore::session::SessionStore::instance().hasActiveSessionToken(t_token)) {
        SGRN_WARN_LOG("AutomatedServiceAuthFilter: Fast-reject unknown token: {}", redactToken(t_token));
        tsp_filter_callback(createTypedResponse(BackendErrorResponse{"Unauthorized: Unknown service token", drogon::k401Unauthorized}));
        return;
    }

    // 2. Fetch session from SessionStore asynchronously
    drogon::async_run([tsp_req, tsp_filter_callback = std::move(tsp_filter_callback),
                          t_filter_chain_callback = std::move(t_filter_chain_callback), t_token]() mutable -> drogon::Task<void> {
        auto session_opt = co_await sgrn::datastore::session::SessionStore::instance().getSession(t_token);
        if (!session_opt.has_value()) {
            SGRN_WARN_LOG("AutomatedServiceAuthFilter: session not found for token");
            tsp_filter_callback(
                createTypedResponse(BackendErrorResponse{"Unauthorized: Session expired or invalid", drogon::k401Unauthorized}));
            co_return;
        }

        const auto& claims = session_opt.value();
        const auto& user = claims.user;

        if (!user.automated_service_id || *user.automated_service_id == 0) {
            SGRN_WARN_LOG("AutomatedServiceAuthFilter: access denied — session is not for an automated service");
            tsp_filter_callback(createTypedResponse(BackendErrorResponse{"Automated Service credentials required", drogon::k403Forbidden}));
            co_return;
        }

        const int32_t automated_service_id = *user.automated_service_id;

        AuthContext auth_ctx;
        auth_ctx.automated_service_id = automated_service_id;
        if (!user.organisation.empty()) {
            auth_ctx.user.organisation = user.organisation;
        }
        if (!user.email.empty()) {
            auth_ctx.user.email = user.email;
        }
        if (claims.session_id != 0) {
            auth_ctx.session_id = claims.session_id;
        }

        tsp_req->attributes()->insert("tenant_id", auth_ctx.user.organisation);
        tsp_req->attributes()->insert("auth_context", std::move(auth_ctx));
        tsp_req->attributes()->insert("session_claims", claims);
        tsp_req->attributes()->insert("automated_service_id", automated_service_id);

        SGRN_INFO_LOG("AutomatedServiceAuthFilter: automated service {} authenticated", automated_service_id);

        t_filter_chain_callback();
    });
}

} // namespace sgrn::datastore::filters
