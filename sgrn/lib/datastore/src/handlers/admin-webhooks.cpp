// admin-webhooks.cpp — Webhook administration.
#include "admin-common.hpp"

#include <sgrn/utils/json-serialization.hpp>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{
struct WebhookResponse {
    int32_t id{0};
    std::string organisation;
    std::string url;
    bool is_active{false};
    std::string created_at;
};

struct WebhookRegistrationResponse {
    int32_t id{0};
    std::string url;
    std::string organisation;
    std::string created_at;
};

struct WebhookDeleteResponse {
    bool success{true};
    std::string message;
};

template <typename T>
Task<HttpResponsePtr> jsonResponse(T t_value, HttpStatusCode t_status) {
    auto response_result = sgrn::json::write(t_value);
    if (response_result.hasError()) {
        co_return createErrorResponse("Failed to serialize webhook response", k500InternalServerError, "AdminApi");
    }
    std::string response_json = std::move(response_result).value();
    co_return createJsonResponse(std::move(response_json), t_status);
}
} // namespace detail

Task<HttpResponsePtr> AdminApiHandler::handleListWebhooks(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();

        auto endpoints = co_await sgrn::datastore::services::WebhookService::instance().getWebhooks(org);
        std::vector<detail::WebhookResponse> list;
        list.reserve(endpoints.size());
        for (const auto& ep : endpoints) {
            list.push_back({ep.id, ep.organisation, ep.url, ep.is_active, ep.created_at});
        }
        co_return co_await detail::jsonResponse(std::move(list), k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed listing webhooks: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterWebhook(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        auto json = tsp_req->getJsonObject();

        if (!json || !json->isMember("url") || !(*json)["url"].isString()) {
            co_return createErrorResponse("url is required", k400BadRequest, "AdminApi");
        }

        std::string url = (*json)["url"].asString();
        std::string secret = json->get("secret", "").asString();
        std::vector<std::string> events = {"user.signin", "user.signout", "service.signin"};

        auto ep_opt = co_await sgrn::datastore::services::WebhookService::instance().createWebhook(org, url, secret, events);
        if (!ep_opt) {
            co_return createErrorResponse("Failed creating webhook subscription", k400BadRequest, "AdminApi");
        }

        co_return co_await detail::jsonResponse(
            detail::WebhookRegistrationResponse{ep_opt->id, ep_opt->url, ep_opt->organisation, ep_opt->created_at}, k201Created);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed registering webhook: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteWebhook(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        int32_t webhook_id = std::stoi(t_id);

        bool deleted = co_await sgrn::datastore::services::WebhookService::instance().deleteWebhook(org, webhook_id);
        if (!deleted) {
            co_return createErrorResponse("Webhook not found", k404NotFound, "AdminApi");
        }

        co_return co_await detail::jsonResponse(detail::WebhookDeleteResponse{true, "Webhook deleted successfully"}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed deleting webhook: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

} // namespace sgrn::datastore::handlers::admin
