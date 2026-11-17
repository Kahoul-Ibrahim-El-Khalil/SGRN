#pragma once

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/json-serialization.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sgrn::datastore::services
{

struct WebhookEndpoint {
    int32_t id{0};
    std::string organisation;
    std::string url;
    std::string secret;
    std::vector<std::string> events;
    bool is_active{true};
    std::string created_at;
};

class WebhookService {
public:
    static WebhookService& instance();

    // Asynchronously dispatch a typed payload to matching subscribers.
    template <typename T>
    void dispatchEvent(const std::string& organisation, const std::string& event_type, const T& payload) {
        auto serialized = sgrn::json::writeOmitNull(payload);
        if (serialized.hasError()) {
            SGRN_ERROR("WebhookService", "Failed to serialize '{}' payload: {}", event_type, serialized.error().message);
            return;
        }
        dispatchSerializedEvent(organisation, event_type, std::move(serialized).value());
    }

    // Database CRUD operations for webhooks
    drogon::Task<std::vector<WebhookEndpoint>> getWebhooks(const std::string& organisation);
    drogon::Task<std::optional<WebhookEndpoint>> createWebhook(
        const std::string& organisation, const std::string& url, const std::string& secret, const std::vector<std::string>& events);
    drogon::Task<bool> deleteWebhook(const std::string& organisation, int32_t webhook_id);

private:
    WebhookService() = default;

    void dispatchSerializedEvent(const std::string& organisation, const std::string& event_type, std::string payload_json);
    std::string calculateHmacSha256(const std::string& secret, const std::string& payload);
    void sendHttpRequest(const std::string& url, const std::string& secret, const std::string& event_type, const std::string& body);
};

} // namespace sgrn::datastore::services
