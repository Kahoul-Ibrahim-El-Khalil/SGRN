#pragma once
#include <drogon/drogon.h>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <regex>

namespace sgrn::datastore::handlers::telemetry
{

class TelemetryHandler : public sgrn::IHandler<TelemetryHandler> {
public:
    TelemetryHandler()
        : IHandler(this, kRoutes) {
    }

    drogon::Task<drogon::HttpResponsePtr> handleIngest(drogon::HttpRequestPtr tsp_http_req);
    drogon::Task<drogon::HttpResponsePtr> handleQuery(drogon::HttpRequestPtr tsp_http_req);

    inline static const std::array<sgrn::IHandler<TelemetryHandler>::route_config, 4> kRoutes = {
        {{"/api/v1/telemetry/ingest", &TelemetryHandler::handleIngest, {drogon::Post},
             {"sgrn::datastore::filters::AutomatedServiceAuthFilter", "sgrn::datastore::filters::DecompressionFilter"}},
            {"/api/v1/telemetry/query", &TelemetryHandler::handleQuery, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},
            {"/api/v1/telemetry/automated-service/query", &TelemetryHandler::handleQuery, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},
            // Compatibility: legacy gateway endpoint.
            {"/api/v1/s7/gateway/ingest", &TelemetryHandler::handleIngest, {drogon::Post},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter", "sgrn::datastore::filters::DecompressionFilter"}}}};
};

} // namespace sgrn::datastore::handlers::telemetry
