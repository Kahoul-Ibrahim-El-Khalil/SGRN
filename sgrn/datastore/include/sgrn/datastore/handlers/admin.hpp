/*sgrn/admin/include/sgrn/api/admin/handler.hpp*/
#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/datastore/services/admin.hpp>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/debug.hpp>
#include <regex>

namespace sgrn::datastore::handlers::admin
{

class AdminApiHandler : public ::sgrn::IHandler<AdminApiHandler> {
public:
    AdminApiHandler()
        : IHandler(this, kRoutes) {
    }

    drogon::Task<drogon::HttpResponsePtr> handleGetStatus(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetUsers(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRegisterUser(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRegisterAutomatedService(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetEndpoints(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetMetaProbeSessions(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handlePostgrestProxyRequest(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleListAutomatedServices(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateAutomatedServiceMetadata(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRotateAutomatedServiceToken(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleListIotObjects(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateIotObjectMetadata(drogon::HttpRequestPtr tsp_req);

private:
    std::string endpoints_cache_;

    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::route_config, 11> kRoutes = {{
        {"/api/v1/admin/status", &AdminApiHandler::handleGetStatus, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/users", &AdminApiHandler::handleGetUsers, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/users/register", &AdminApiHandler::handleRegisterUser, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/automated-services/register", &AdminApiHandler::handleRegisterAutomatedService, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/endpoints", &AdminApiHandler::handleGetEndpoints, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},
        {"/api/v1/admin/automated-services", &AdminApiHandler::handleListAutomatedServices, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/automated-services/{id}/metadata", &AdminApiHandler::handleUpdateAutomatedServiceMetadata, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/automated-services/rotate-token", &AdminApiHandler::handleRotateAutomatedServiceToken, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/telemetry/objects", &AdminApiHandler::handleListIotObjects, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/telemetry/objects/{id}/metadata", &AdminApiHandler::handleUpdateIotObjectMetadata, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"~^/api/v1/postgrest/.*", &AdminApiHandler::handlePostgrestProxyRequest,
            {drogon::Get, drogon::Post, drogon::Put, drogon::Patch, drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};
};
} // namespace sgrn::datastore::handlers::admin
