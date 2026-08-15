#include <sgrn/datastore/handlers/telemetry.hpp>
#include <sgrn/datastore/services/telemetry.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <json/json.h>

namespace sgrn::datastore::handlers::telemetry
{

drogon::Task<drogon::HttpResponsePtr> TelemetryHandler::handleIngest(drogon::HttpRequestPtr tsp_http_req) {
    int32_t automated_service_id = -1;
    try {
        automated_service_id = tsp_http_req->getAttributes()->get<int32_t>("automated_service_id");
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("TelemetryHandler", "Failed to get automated_service_id: {}", e.what());
        automated_service_id = -1;
    }

    const auto& json_ptr = tsp_http_req->getJsonObject();
    if (!json_ptr || !json_ptr->isObject()) {
        co_return sgrn::createJsonErrorResponse("Invalid JSON payload", drogon::k400BadRequest);
    }

    // Acknowledge Schema definitions (SDK initialization)
    if (json_ptr->get("type", "").asString() == "SCHEMA") {
        Json::Value resp;
        resp["message"] = "Schema acknowledged";
        co_return sgrn::createJsonResponse(std::move(resp), drogon::k200OK);
    }

    const auto res = co_await sgrn::datastore::services::telemetry::TelemetryService::ingestHeartbeat(json_ptr, automated_service_id);

    if (res.ok) {
        Json::Value resp;
        resp["message"] = res.message;
        co_return sgrn::createJsonResponse(std::move(resp), res.status);
    } else {
        co_return sgrn::createJsonErrorResponse(res.message, res.status);
    }
}

drogon::Task<drogon::HttpResponsePtr> TelemetryHandler::handleQuery(drogon::HttpRequestPtr tsp_http_req) {
    co_return sgrn::createJsonErrorResponse("Historical telemetry query is disabled. Use raw file storage.", drogon::k501NotImplemented);
}

} // namespace sgrn::datastore::handlers::telemetry
