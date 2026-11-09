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

} // namespace sgrn::datastore::handlers::admin
