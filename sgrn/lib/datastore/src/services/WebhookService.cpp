#include <sgrn/datastore/services/WebhookService.hpp>

#include <drogon/HttpClient.h>
#include <drogon/orm/CoroMapper.h>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/jsoncpp.hpp>

namespace sgrn::datastore::services
{
using drogon::Task;

using sgrn::datastore::core::getDbClient;

namespace
{
struct WebhookEnvelope {
    std::string event;
    std::string timestamp;
    sgrn::json::RawJson data;
};

struct WebhookDispatchRow {
    std::string url;
    std::optional<std::string> secret;
};

struct WebhookCreatedRow {
    int32_t id{0};
    std::string created_at;
};

static constexpr char kDispatchWebhooksQuery[] = "SELECT url, secret FROM core.webhooks "
                                                 "WHERE organisation = $1 AND is_active = true "
                                                 "AND (events IS NULL OR $2 = ANY(events))";
static constexpr char kCreateWebhookQuery[] =
    "INSERT INTO core.webhooks (organisation, url, secret) VALUES ($1, $2, $3) RETURNING id, created_at";
} // namespace

WebhookService& WebhookService::instance() {
    static WebhookService inst;
    return inst;
}

std::string WebhookService::calculateHmacSha256(const std::string& secret, const std::string& payload) {
    auto res = sgrn::utils::computeHmacSha256(secret, payload, sgrn::utils::HashEncoding::hex);
    return res.hasValue() ? *res : std::string{};
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

void WebhookService::dispatchSerializedEvent(const std::string& organisation, const std::string& event_type, std::string payload_json) {
    drogon::async_run([this, organisation, event_type, payload_json = std::move(payload_json)]() -> Task<void> {
        try {
            auto db_res = getDbClient();
            if (db_res.hasError())
                co_return;
            auto db = db_res.value();

            // Only deliver to webhooks that either have no event filter (events IS NULL)
            // or have explicitly subscribed to this event type.
            auto rows = co_await db->execSqlCoro(kDispatchWebhooksQuery, organisation, event_type);

            WebhookEnvelope envelope{
                event_type, trantor::Date::now().toFormattedString(false), sgrn::json::RawJson{std::move(payload_json)}};
            auto body_result = sgrn::json::write(envelope);
            if (body_result.hasError()) {
                SGRN_ERROR("WebhookService", "Failed to serialize '{}' envelope: {}", event_type, body_result.error().message);
                co_return;
            }
            std::string body = std::move(body_result).value();

            for (const auto& row : sgrn::orm::fromResult<WebhookDispatchRow, kDispatchWebhooksQuery>(rows)) {
                sendHttpRequest(row.url, row.secret.value_or(""), event_type, body);
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

    co_return sgrn::orm::fromResultByName<WebhookEndpoint>(rows);
}

Task<std::optional<WebhookEndpoint>> WebhookService::createWebhook(
    const std::string& organisation, const std::string& url, const std::string& secret, const std::vector<std::string>& events) {
    auto db_res = getDbClient();
    if (db_res.hasError()) {
        co_return std::nullopt;
    }
    auto db = db_res.value();

    auto rows = co_await db->execSqlCoro(kCreateWebhookQuery, organisation, url, secret);

    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto row = sgrn::orm::fromRowByName<WebhookCreatedRow>(rows[0]);
    WebhookEndpoint ep{.id = row.id,
        .organisation = organisation,
        .url = url,
        .secret = secret,
        .events = {},
        .is_active = true,
        .created_at = row.created_at};
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
