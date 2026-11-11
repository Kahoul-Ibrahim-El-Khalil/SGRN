// admin-status.cpp — Status, endpoint discovery and meta-probe sessions..
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

Task<HttpResponsePtr> AdminApiHandler::handleGetStatus(HttpRequestPtr tsp_req) {
    Json::Value status;
    status["status"] = "operational";
    status["version"] = "1.0.0";
    status["timestamp"] = trantor::Date::now().toFormattedString(false);
    co_return createJsonResponse(status, k200OK);
}

Task<HttpResponsePtr> AdminApiHandler::handleGetEndpoints(HttpRequestPtr tsp_req) {
    Json::Value endpoints = Json::arrayValue;
    // Basic discovery - could be expanded to list all registered routes
    endpoints.append("/api/v1/admin/status");
    endpoints.append("/api/v1/admin/users");
    endpoints.append("/api/v1/admin/metaprobe/sessions");
    endpoints.append("/api/v1/admin/storage/overview");
    endpoints.append("/api/v1/admin/storage/orphans");
    endpoints.append("/api/v1/admin/storage/orphans/purge");
    endpoints.append("/api/v1/admin/storage/search");
    endpoints.append("/api/v1/admin/system/config");
    endpoints.append("/api/v1/admin/storage/formats");
    endpoints.append("/api/v1/admin/quotas");
    endpoints.append("/api/v1/admin/permissions");
    endpoints.append("/api/v1/admin/analytics/overview");
    endpoints.append("/api/v1/admin/analytics/breakdown");
    co_return createJsonResponse(endpoints, k200OK);
}

Task<HttpResponsePtr> AdminApiHandler::handleGetMetaProbeSessions(HttpRequestPtr tsp_req) {
    try {
        Json::Value sessions = co_await sgrn::datastore::session::SessionStore::instance().getActiveSessions();
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        if (!is_platform && !session_org.empty()) {
            Json::Value filtered = Json::arrayValue;
            if (sessions.isArray()) {
                for (const auto& s : sessions) {
                    if (s.isMember("organisation") && s["organisation"].asString() == session_org) {
                        filtered.append(s);
                    } else if (s.isMember("user") && s["user"].isMember("organisation") &&
                               s["user"]["organisation"].asString() == session_org) {
                        filtered.append(s);
                    }
                }
            }
            co_return createJsonResponse(filtered, k200OK);
        }

        co_return createJsonResponse(sessions, k200OK);
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

        Json::Value details(Json::objectValue);
        details["token_prefix"] = token.substr(0, 8);
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "session.revoked",
            "session", token, tsp_req->getPeerAddr().toIp(), details, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["revoked_session"] = token;
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to revoke session: {}", e.what());
        co_return createErrorResponse(std::string("Failed to revoke session: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
