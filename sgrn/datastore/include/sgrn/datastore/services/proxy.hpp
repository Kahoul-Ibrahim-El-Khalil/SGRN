#pragma once
#include <drogon/drogon.h>
#include <json/json.h>
#include <regex>

namespace sgrn::datastore::services::proxy
{

class PostgrestProxyService {
public:
    /**
     * @brief Proxies a request to PostgREST after injecting multi-tenant isolation filters.
     *
     * @param t_req The original incoming request.
     * @param t_table_name The target PostgREST table/view.
     * @return drogon::Task<drogon::HttpResponsePtr> The PostgREST response or an error response.
     */
    static drogon::Task<drogon::HttpResponsePtr> proxyToPostgrest(drogon::HttpRequestPtr tsp_req, const std::string& t_table_name);

private:
    /**
     * @brief Injects organisation_id and optionally user_id/automated_service_id filters based on the session.
     */
    static bool injectIsolationFilters(drogon::HttpRequestPtr tsp_req, const Json::Value& t_session);
};

} // namespace sgrn::datastore::services::proxy
