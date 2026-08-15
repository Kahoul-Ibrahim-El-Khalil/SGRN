#pragma once
#include <drogon/drogon.h>
#include <json/json.h>
#include <memory>
#include <string>

namespace sgrn::datastore::services::telemetry
{

class TelemetryService {
public:
    struct IngestResult {
        bool ok{false};
        std::string message;
        drogon::HttpStatusCode status{drogon::k500InternalServerError};
    };

    /**
     * @brief Ingests a heartbeat/state update from an automated service.
     * This ensures the object exists and updates its status/metadata.
     * Historical data is NO LONGER persisted.
     */
    static drogon::Task<IngestResult> ingestHeartbeat(const std::shared_ptr<Json::Value>& tsp_json_ptr, int32_t t_automated_service_id);
};

} // namespace sgrn::datastore::services::telemetry
