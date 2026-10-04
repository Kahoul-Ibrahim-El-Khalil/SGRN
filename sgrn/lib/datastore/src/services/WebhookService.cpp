#include <sgrn/datastore/services/WebhookService.hpp>

#include <drogon/HttpClient.h>
#include <drogon/orm/CoroMapper.h>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/jsoncpp.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <iomanip>
#include <sstream>

namespace sgrn::datastore::services
{
using drogon::Task;

using sgrn::datastore::core::getDbClient;
WebhookService& WebhookService::instance() {
    static WebhookService inst;
    return inst;
}

std::string WebhookService::calculateHmacSha256(const std::string& secret, const std::string& payload) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size(), hash, &len);

    std::stringstream ss;
    for (unsigned int i = 0; i < len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

void WebhookService::sendHttpRequest(
    const std::string& url, const std::string& secret, const std::string& event_type, const std::string& body) {
    try {
        auto client = drogon::HttpClient::newHttpClient(url);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(body);
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->addHeader("X-SGRN-Event", event_type);
        if (!secret.empty()) {
            req->addHeader("X-SGRN-Signature", calculateHmacSha256(secret, body));
        }

        client->sendRequest(req, [url, event_type](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
            if (result == drogon::ReqResult::Ok && response) {
                SGRN_DEBUG("WebhookService", "Dispatched event '{}' to {} -> status {}", event_type, url,
                    static_cast<int>(response->statusCode()));
            } else {
                SGRN_WARN(
                    "WebhookService", "Failed dispatching event '{}' to {}: result code {}", event_type, url, static_cast<int>(result));
            }
        });
    } catch (const std::exception& e) {
        SGRN_ERROR("WebhookService", "Exception sending webhook request to {}: {}", url, e.what());
    }
}

void WebhookService::dispatchEvent(const std::string& organisation, const std::string& event_type, const Json::Value& payload) {
    drogon::async_run([this, organisation, event_type, payload]() -> Task<void> {
        try {
            auto db_res = getDbClient();
            if (db_res.hasError())
                co_return;
            auto db = db_res.value();

            // Only deliver to webhooks that either have no event filter (events IS NULL)
            // or have explicitly subscribed to this event type.
            auto rows = co_await db->execSqlCoro("SELECT url, secret FROM core.webhooks "
                                                 "WHERE organisation = $1 AND is_active = true "
                                                 "AND (events IS NULL OR $2 = ANY(events))",
                organisation, event_type);

            Json::Value envelope;
            envelope["event"] = event_type;
            envelope["timestamp"] = trantor::Date::now().toFormattedString(false);
            envelope["data"] = payload;

            Json::StreamWriterBuilder writer;
            std::string body = Json::writeString(writer, envelope);

            for (const auto& row : rows) {
                std::string url = row["url"].as<std::string>();
                std::string secret = row["secret"].isNull() ? "" : row["secret"].as<std::string>();
                sendHttpRequest(url, secret, event_type, body);
            }
        } catch (const std::exception& e) {
            SGRN_ERROR("WebhookService", "Error dispatching webhook event: {}", e.what());
        }
    });
}

Task<std::vector<WebhookEndpoint>> WebhookService::getWebhooks(const std::string& organisation) {
    std::vector<WebhookEndpoint> result;
    auto db_res = getDbClient();
    if (db_res.hasError())
        co_return result;
    auto db = db_res.value();

    auto rows = co_await db->execSqlCoro(
        "SELECT id, organisation, url, secret, is_active, created_at FROM core.webhooks WHERE organisation = $1 ORDER BY id DESC",
        organisation);

    for (const auto& row : rows) {
        WebhookEndpoint ep;
        ep.id = row["id"].as<int32_t>();
        ep.organisation = row["organisation"].as<std::string>();
        ep.url = row["url"].as<std::string>();
        ep.secret = row["secret"].as<std::string>();
        ep.is_active = row["is_active"].as<bool>();
        ep.created_at = row["created_at"].as<std::string>();
        result.push_back(ep);
    }
    co_return result;
}

Task<std::optional<WebhookEndpoint>> WebhookService::createWebhook(
    const std::string& organisation, const std::string& url, const std::string& secret, const std::vector<std::string>& events) {
    auto db_res = getDbClient();
    if (db_res.hasError()) {
        co_return std::nullopt;
    }
    auto db = db_res.value();

    auto rows = co_await db->execSqlCoro(
        "INSERT INTO core.webhooks (organisation, url, secret) VALUES ($1, $2, $3) RETURNING id, created_at", organisation, url, secret);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    WebhookEndpoint ep{.id = rows[0]["id"].as<int32_t>(),
        .organisation = organisation,
        .url = url,
        .secret = secret,
        .events = {},
        .is_active = true,
        .created_at = rows[0]["created_at"].as<std::string>()};
    co_return ep;
}

Task<bool> WebhookService::deleteWebhook(const std::string& organisation, int32_t webhook_id) {
    auto db_res = getDbClient();
    if (db_res.hasError()) {
        co_return false;
    }
    auto db = db_res.value();

    auto res = co_await db->execSqlCoro("DELETE FROM core.webhooks WHERE organisation = $1 AND id = $2", organisation, webhook_id);
    co_return res.affectedRows() > 0;
}

} // namespace sgrn::datastore::services
