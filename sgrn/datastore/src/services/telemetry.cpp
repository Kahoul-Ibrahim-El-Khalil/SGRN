#include <drogon/drogon.h>
#include <sgrn/datastore/services/telemetry.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/jsoncpp.hpp>
#include <cctype>
#include <regex>

#ifdef DEBUG_TELEMETRY_SERVICE
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("TelemetryService", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("TelemetryService", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("TelemetryService", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("TelemetryService", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::services::telemetry
{
using namespace drogon;

namespace
{
static std::string canonicalMac(const std::string& t_mac) {
    std::string out;
    out.reserve(t_mac.size());
    for (char c : t_mac) {
        if (std::isspace(static_cast<unsigned char>(c)))
            continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

struct Identity {
    std::optional<std::string> object_name;
    std::optional<int32_t> plc_id;
    std::optional<std::string> mac;
};

Identity extractIdentity(const Json::Value& t_json) {
    Identity id;
    if (t_json.isMember("object_name") && t_json["object_name"].isString()) {
        id.object_name = t_json["object_name"].asString();
    }
    if (t_json.isMember("plc_id") && t_json["plc_id"].isInt()) {
        id.plc_id = t_json["plc_id"].asInt();
    }
    if (t_json.isMember("mac") && t_json["mac"].isString()) {
        id.mac = canonicalMac(t_json["mac"].asString());
    } else if (t_json.isMember("mac_address") && t_json["mac_address"].isString()) {
        id.mac = canonicalMac(t_json["mac_address"].asString());
    }

    // If not found at root, check inside 'data' if it's a known SDK batch format
    if (!id.object_name && !id.plc_id && !id.mac && t_json.isMember("data")) {
        const auto& data = t_json["data"];
        if (data.isArray() && !data.empty()) {
            const auto& first = data[0u];
            if (first.isMember("object_name") && first["object_name"].isString()) {
                id.object_name = first["object_name"].asString();
            }
            if (first.isMember("plc_id") && first["plc_id"].isInt()) {
                id.plc_id = first["plc_id"].asInt();
            }
        }
    }
    return id;
}
} // namespace

Task<TelemetryService::IngestResult> TelemetryService::ingestHeartbeat(
    const std::shared_ptr<Json::Value>& tsp_json_ptr, int32_t t_automated_service_id) {
    auto db_client = drogon::app().getDbClient();
    if (db_client == nullptr) {
        ERROR_LOG("Database unavailable");
        co_return {.ok = false, .message = "Internal Backend Error", .status = drogon::k503ServiceUnavailable};
    }

    try {
        if (t_automated_service_id <= 0) {
            co_return {.ok = false, .message = "Unauthorized", .status = drogon::k401Unauthorized};
        }

        // Fetch service organisation and domain
        std::string org;
        std::string domain;
        try {
            auto svc_res = co_await db_client->execSqlCoro(
                "SELECT organisation, domain FROM core.automated_services WHERE id = $1", t_automated_service_id);
            if (svc_res.empty()) {
                co_return {.ok = false, .message = "Automated service not found", .status = drogon::k401Unauthorized};
            }
            org = svc_res[0]["organisation"].as<std::string>();
            domain = svc_res[0]["domain"].isNull() ? "" : svc_res[0]["domain"].as<std::string>();
        } catch (const std::exception& e) {
            ERROR_LOG("Failed to fetch service details: {}", e.what());
            co_return {.ok = false, .message = "Internal Backend Error", .status = drogon::k500InternalServerError};
        }

        // Resolve or Upsert the actual telemetry.object
        Identity id = extractIdentity(*tsp_json_ptr);

        if (!id.object_name && !id.plc_id && !id.mac) {
            co_return {.ok = false, .message = "Missing object identifier (object_name, plc_id, or mac)", .status = drogon::k400BadRequest};
        }

        int32_t object_id = -1;

        if (id.object_name.has_value()) {
            auto res = co_await db_client->execSqlCoro("SELECT id FROM telemetry.\"object\" WHERE automated_service_id = $1 AND name = $2",
                t_automated_service_id, *id.object_name);
            if (res.empty()) {
                co_return {.ok = false, .message = "Unknown 'object_name'", .status = drogon::k404NotFound};
            }
            object_id = res[0]["id"].as<int32_t>();
        } else if (id.mac.has_value()) {
            // UPSERT by MAC
            const std::string name = id.plc_id.has_value() ? ("plc-" + std::to_string(*id.plc_id)) : ("device-" + *id.mac);
            auto res = co_await db_client->execSqlCoro(
                "INSERT INTO telemetry.\"object\" (automated_service_id, name, metadata, status, organisation, domain) "
                "VALUES ($1, $2, jsonb_build_object('mac', $3, 'plc_id', $4), 'online', $5, $6) "
                "ON CONFLICT (automated_service_id, name) "
                "DO UPDATE SET metadata = telemetry.\"object\".metadata || EXCLUDED.metadata, status = 'online' "
                "RETURNING id",
                t_automated_service_id, name, *id.mac, id.plc_id ? Json::Value(*id.plc_id) : Json::Value(Json::nullValue), org, domain);

            if (!res.empty())
                object_id = res[0]["id"].as<int32_t>();
        } else if (id.plc_id.has_value()) {
            // UPSERT by PLC_ID
            const std::string name = "plc-" + std::to_string(*id.plc_id);
            auto res = co_await db_client->execSqlCoro(
                "INSERT INTO telemetry.\"object\" (automated_service_id, name, metadata, status, organisation, domain) "
                "VALUES ($1, $2, jsonb_build_object('plc_id', $3), 'online', $4, $5) "
                "ON CONFLICT (automated_service_id, name) "
                "DO UPDATE SET metadata = telemetry.\"object\".metadata || EXCLUDED.metadata, status = 'online' "
                "RETURNING id",
                t_automated_service_id, name, *id.plc_id, org, domain);

            if (!res.empty())
                object_id = res[0]["id"].as<int32_t>();
        }

        if (object_id <= 0) {
            co_return {.ok = false, .message = "Failed to provision/resolve object", .status = drogon::k500InternalServerError};
        }

        co_return {.ok = true, .message = "Heartbeat acknowledged", .status = drogon::k200OK};

    } catch (const std::exception& e) {
        ERROR_LOG("Exception during heartbeat: {}", e.what());
        co_return {.ok = false, .message = "Internal Backend Error", .status = drogon::k500InternalServerError};
    }
}

} // namespace sgrn::datastore::services::telemetry
