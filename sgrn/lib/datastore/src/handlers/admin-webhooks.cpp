// admin-webhooks.cpp — Webhook administration.
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

Task<HttpResponsePtr> AdminApiHandler::handleListWebhooks(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();

        auto endpoints = co_await sgrn::datastore::services::WebhookService::instance().getWebhooks(org);
        Json::Value list = Json::arrayValue;
        for (const auto& ep : endpoints) {
            Json::Value item;
            item["id"] = ep.id;
            item["organisation"] = ep.organisation;
            item["url"] = ep.url;
            item["is_active"] = ep.is_active;
            item["created_at"] = ep.created_at;
            list.append(item);
        }
        co_return createJsonResponse(list, k200OK);
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

        Json::Value resp;
        resp["id"] = ep_opt->id;
        resp["url"] = ep_opt->url;
        resp["organisation"] = ep_opt->organisation;
        resp["created_at"] = ep_opt->created_at;
        co_return createJsonResponse(resp, k201Created);
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

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Webhook deleted successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed deleting webhook: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

} // namespace sgrn::datastore::handlers::admin
