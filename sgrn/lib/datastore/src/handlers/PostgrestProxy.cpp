#include <sgrn/datastore/handlers/PostgrestProxy.hpp>
#include <sgrn/datastore/plugins/postgrest/PostgrestClient.hpp>
#include <sgrn/datastore/services/proxy.hpp>
#include <sgrn/debug.hpp>

namespace sgrn::datastore::handlers::query
{

drogon::Task<drogon::HttpResponsePtr> PostgrestProxyHandler::handleProxyRequest(drogon::HttpRequestPtr tsp_http_req) {
    std::string path = tsp_http_req->getPath();
    std::string table = "files"; // fallback

    static constexpr std::string_view kProxyPrefix = "/api/v1/postgrest";

    if (path.length() > kProxyPrefix.length() + 1) {
        // Extract suffix after "/api/v1/postgrest/"
        std::string suffix = path.substr(kProxyPrefix.length() + 1);
        if (!suffix.empty()) {
            // Replace '/' with '_' to form PostgREST view names
            std::replace(suffix.begin(), suffix.end(), '/', '_');
            table = suffix;
        }
    }

    co_return co_await sgrn::datastore::services::proxy::PostgrestProxyService::proxyToPostgrest(tsp_http_req, std::move(table));
}

} // namespace sgrn::datastore::handlers::query
