// admin-webhooks.cpp — Webhook administration.
#include "admin-common.hpp"

#include <sgrn/datastore/wire/wire.hpp>
#include <sgrn/utils/json-serialization.hpp>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{
using WebhookResponse = ::sgrn::datastore::admin::Webhook;

using RegisterWebhookRequest = ::sgrn::datastore::admin::RegisterWebhookRequest;

using WebhookRegistrationResponse = ::sgrn::datastore::admin::WebhookRegistration;

using WebhookDeleteResponse = ::sgrn::datastore::common::Message;

} // namespace detail

Task<HttpResponsePtr> AdminApiHandler::handleListWebhooks(HttpRequestPtr tsp_req) {
    try {
        const std::string org = sgrn::datastore::getSessionOrganisation(tsp_req);

        auto endpoints = co_await sgrn::datastore::services::WebhookService::instance().getWebhooks(org);
        std::vector<detail::WebhookResponse> list;
        list.reserve(endpoints.size());
        for (const auto& ep : endpoints) {
            list.push_back({ep.id, ep.organisation, ep.url, ep.is_active, ep.created_at});
        }
        co_return createTypedResponse(tsp_req, list, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed listing webhooks: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterWebhook(HttpRequestPtr tsp_req) {
    try {
        const std::string org = sgrn::datastore::getSessionOrganisation(tsp_req);
        auto body_res = parseTypedRequest<detail::RegisterWebhookRequest>(tsp_req);
        if (body_res.hasError() || body_res->url.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"url is required", k400BadRequest, "AdminApi"});
        }

        const auto& req = body_res.value();
        const std::vector<std::string>& events =
            req.events.empty() ? std::vector<std::string>{"user.signin", "user.signout", "service.signin"} : req.events;

        auto ep_opt = co_await sgrn::datastore::services::WebhookService::instance().createWebhook(org, req.url, req.secret, events);
        if (!ep_opt) {
            co_return createTypedResponse(BackendErrorResponse{"Failed creating webhook subscription", k400BadRequest, "AdminApi"});
        }

        co_return createTypedResponse(
            tsp_req, detail::WebhookRegistrationResponse{ep_opt->id, ep_opt->url, ep_opt->organisation, ep_opt->created_at}, k201Created);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed registering webhook: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteWebhook(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const std::string org = sgrn::datastore::getSessionOrganisation(tsp_req);
        int32_t webhook_id = std::stoi(t_id);

        bool deleted = co_await sgrn::datastore::services::WebhookService::instance().deleteWebhook(org, webhook_id);
        if (!deleted) {
            co_return createTypedResponse(BackendErrorResponse{"Webhook not found", k404NotFound, "AdminApi"});
        }

        co_return createTypedResponse(tsp_req, detail::WebhookDeleteResponse{"Webhook deleted successfully"}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed deleting webhook: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    }
}

} // namespace sgrn::datastore::handlers::admin
