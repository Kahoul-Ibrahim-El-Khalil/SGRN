#include <fmt/core.h>
#include <sgrn/datastore/plugins/postgrest/PostgrestClient.hpp>
#include <sgrn/datastore/services/proxy.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <regex>

namespace sgrn::datastore::services::proxy
{
using drogon::Task;
using namespace drogon;
using namespace sgrn::datastore;

Task<HttpResponsePtr> PostgrestProxyService::proxyToPostgrest(HttpRequestPtr tsp_req, const std::string& t_table_name) {
    if (!tsp_req->attributes()->find("session_json")) {
        co_return sgrn::createErrorResponse({"Authentication", "Unauthorized: Session missing"}, k401Unauthorized);
    }
    const Json::Value& t_session = tsp_req->attributes()->get<Json::Value>("session_json");

    if (injectIsolationFilters(tsp_req, t_session) == false) {
        co_return sgrn::createErrorResponse({"Authentication", "Unauthorized: Organization identity not found in session"}, k403Forbidden);
    }

    auto* p_plugin = drogon::app().getPlugin<sgrn::datastore::plugins::PostgrestClient>();
    if (p_plugin == nullptr) {
        co_return sgrn::createErrorResponse(
            {"Postgrest", "Internal Proxy Error: PostgrestClient plugin not found"}, k500InternalServerError);
    }

    // Set the path to the PostgREST table
    tsp_req->setPath("/" + t_table_name);

    // Ensure session_id=eq.current is expanded
    auto params = tsp_req->getParameters();
    if (params.count("session_id") && params["session_id"] == "eq.current") {
        if (t_session.isMember("session_id") && t_session["session_id"].isInt64()) {
            tsp_req->setParameter("session_id", fmt::format("eq.{}", t_session["session_id"].asInt64()));
        }
    }

    auto resp = co_await p_plugin->sendRequest(tsp_req);
    co_return resp;
}

bool PostgrestProxyService::injectIsolationFilters(HttpRequestPtr tsp_req, const Json::Value& t_session) {
    std::string org = "";

    if (t_session.isMember("user")) {
        const Json::Value& user = t_session["user"];

        // 1. User session / Automated Service session
        if (user.isMember("organisation") && user["organisation"].isString()) {
            org = user["organisation"].asString();

            if (user.isMember("role") && user["role"].isMember("name")) {
                const std::string role = user["role"]["name"].asString();
                if (role != "admin" && role != "automated_service") {
                    tsp_req->setParameter("user_id", "eq." + std::to_string(user["id"].asInt()));
                }
            }

            if (user.isMember("automated_service_id")) {
                tsp_req->setParameter("automated_service_id", "eq." + std::to_string(user["automated_service_id"].asInt()));
            }
        }
    }

    if (!org.empty()) {
        tsp_req->setParameter("organisation", "eq." + org);
        return true;
    }

    return false;
}

} // namespace sgrn::datastore::services::proxy
