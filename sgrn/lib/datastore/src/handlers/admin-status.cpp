// admin-status.cpp — Status, endpoint discovery and meta-probe sessions..
#include "admin-common.hpp"

#include <sgrn/utils/json-serialization.hpp>

namespace sgrn::datastore::handlers::admin::detail
{
struct StatusResponse {
    std::string status;
    std::string version;
    std::string timestamp;
};

struct EndpointsResponse {
    std::vector<std::string> endpoints;
};

struct SessionRevokedAuditDetails {
    std::string token_prefix;
};

struct SessionRevokedResponse {
    bool success{true};
    std::string revoked_session;
};

struct ActiveSessionResponse {
    std::string token;
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    std::string ip;
    std::string created_at;
    std::string expires_at;
    std::string actor_type;
    std::string actor_name;
};
} // namespace sgrn::datastore::handlers::admin::detail

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

Task<HttpResponsePtr> AdminApiHandler::handleGetStatus(HttpRequestPtr tsp_req) {
    detail::StatusResponse status{
        .status = "operational",
        .version = "1.0.0",
        .timestamp = trantor::Date::now().toFormattedString(false),
    };
    auto response_result = sgrn::json::write(status);
    if (response_result.hasError()) {
        co_return createErrorResponse("Failed to serialize status response", k500InternalServerError, "AdminApi");
    }
    std::string response_json = std::move(response_result).value();
    co_return createJsonResponse(std::move(response_json), k200OK);
}

Task<HttpResponsePtr> AdminApiHandler::handleGetEndpoints(HttpRequestPtr tsp_req) {
    // Basic discovery - could be expanded to list all registered routes
    detail::EndpointsResponse endpoints{{
        "/api/v1/admin/status",
        "/api/v1/admin/users",
        "/api/v1/admin/metaprobe/sessions",
        "/api/v1/admin/storage/overview",
        "/api/v1/admin/storage/orphans",
        "/api/v1/admin/storage/orphans/purge",
        "/api/v1/admin/storage/search",
        "/api/v1/admin/system/config",
        "/api/v1/admin/storage/formats",
        "/api/v1/admin/quotas",
        "/api/v1/admin/permissions",
        "/api/v1/admin/analytics/overview",
        "/api/v1/admin/analytics/breakdown",
    }};
    auto response_result = sgrn::json::write(endpoints.endpoints);
    if (response_result.hasError()) {
        co_return createErrorResponse("Failed to serialize endpoint response", k500InternalServerError, "AdminApi");
    }
    std::string response_json = std::move(response_result).value();
    co_return createJsonResponse(std::move(response_json), k200OK);
}

Task<HttpResponsePtr> AdminApiHandler::handleGetMetaProbeSessions(HttpRequestPtr tsp_req) {
    try {
        const auto sessions = co_await sgrn::datastore::session::SessionStore::instance().getActiveSessions();
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        std::vector<detail::ActiveSessionResponse> response;
        response.reserve(sessions.size());
        for (const auto& session : sessions) {
            if (!is_platform && !session_org.empty() && session.organisation != session_org) {
                continue;
            }
            response.push_back({session.token, session.user_id, session.automated_service_id, session.ip, session.created_at,
                session.expires_at, session.actor_type, session.actor_name});
        }
        co_return createTypedJsonResponse(response, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to get live sessions: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

// DELETE /api/v1/admin/metaprobe/sessions/{token} — terminate one live
// session. Org admins may only revoke sessions of their own organisation;
// platform admins any. The victim instance evicts via the shared
// core.session_notify_queue 'revoke' action, exactly like bulk revocations.
Task<HttpResponsePtr> AdminApiHandler::handleRevokeSession(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);
        const std::string token = sgrn::utils::strings::trim(std::move(t_id));
        if (token.empty()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT s.token::text, u.organisation AS user_org, a.organisation AS service_org "
            "FROM core.sessions s "
            "LEFT JOIN core.users u ON u.id = s.user_id "
            "LEFT JOIN core.automated_services a ON a.id = s.automated_service_id "
            "WHERE s.token = $1::uuid AND s.terminated_at IS NULL",
            {token});
        if (res.empty()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        const std::string owner_org = res[0]["user_org"].isNull()
                                          ? (res[0]["service_org"].isNull() ? "" : res[0]["service_org"].as<std::string>())
                                          : res[0]["user_org"].as<std::string>();
        if (!is_platform && owner_org != ctx.organisation) {
            // missing OR other tenant — same answer
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        // revokeSession stamps terminated_at, evicts the local RAM copy,
        // fires the user.signout webhook and broadcasts the 'revoke' action
        // so every cluster instance drops the token.
        co_await sgrn::datastore::session::SessionStore::instance().revokeSession(token, "admin_revoked");

        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "session.revoked",
            "session", token, tsp_req->getPeerAddr().toIp(), detail::SessionRevokedAuditDetails{token.substr(0, 8)}, "success");

        co_return createTypedJsonResponse(detail::SessionRevokedResponse{true, token}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to revoke session: {}", e.what());
        co_return createErrorResponse(std::string("Failed to revoke session: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
