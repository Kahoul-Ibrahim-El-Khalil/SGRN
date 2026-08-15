#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <regex>

namespace sgrn::datastore::handlers::query
{

/**
 * @brief Secure Proxy for PostgREST
 *
 * Intercepts requests to /api/v1/postgrest/ resource, validates user session,
 * and forwards to PostgREST while injecting organization-based filters.
 */
class PostgrestProxyHandler : public sgrn::IHandler<PostgrestProxyHandler> {
public:
    PostgrestProxyHandler()
        : IHandler(this, kRoutes) {
        SGRN_INFO_LOG("PostgrestProxyHandler initialized and routes registered");
    }

    /**
     * @brief Proxy GET requests to PostgREST
     *
     * Injects organisation_id=eq.{ID} into the query parameters.
     */
    drogon::Task<drogon::HttpResponsePtr> handleProxyRequest(drogon::HttpRequestPtr tsp_http_req);

private:
    static inline const std::array<sgrn::IHandler<PostgrestProxyHandler>::route_config, 6> kRoutes = {
        {{"/api/v1/postgrest/telemetry/objects", &PostgrestProxyHandler::handleProxyRequest, {drogon::Get},
             {"sgrn::datastore::filters::UserAuthFilter"}},
            {"/api/v1/postgrest/telemetry/data", &PostgrestProxyHandler::handleProxyRequest, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},
            {"/api/v1/postgrest/storage/files", &PostgrestProxyHandler::handleProxyRequest, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/postgrest/automated-service/telemetry/objects", &PostgrestProxyHandler::handleProxyRequest, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},
            {"/api/v1/postgrest/automated-service/telemetry/data", &PostgrestProxyHandler::handleProxyRequest, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},
            {"/api/v1/postgrest/automated-service/storage/files", &PostgrestProxyHandler::handleProxyRequest, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}}}};
};

} // namespace sgrn::datastore::handlers::query
