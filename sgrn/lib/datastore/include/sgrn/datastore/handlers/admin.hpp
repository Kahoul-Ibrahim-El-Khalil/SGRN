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
#include <stdexcept>
#include <string>

namespace sgrn::datastore::handlers::admin
{

class AdminApiHandler : public ::sgrn::IHandler<AdminApiHandler> {
public:
    AdminApiHandler()
        : IHandler(this, kRoutes, kItemRoutes) {
    }

    drogon::Task<drogon::HttpResponsePtr> handleGetStatus(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetUsers(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRegisterUser(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRegisterAutomatedService(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetEndpoints(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetMetaProbeSessions(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateAutomatedServiceMetadata(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleRotateAutomatedServiceToken(drogon::HttpRequestPtr tsp_req);

    // Permission System
    drogon::Task<drogon::HttpResponsePtr> handleListPermissions(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGrantPermission(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRevokePermission(drogon::HttpRequestPtr tsp_req);

    // Dynamic RBAC Roles
    drogon::Task<drogon::HttpResponsePtr> handleGetRoles(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleCreateRole(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateRole(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteRole(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleAssignRole(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRevokeRole(drogon::HttpRequestPtr tsp_req);

    // Audit System
    drogon::Task<drogon::HttpResponsePtr> handleGetAuditLogs(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handlePurgeAuditLogs(drogon::HttpRequestPtr tsp_req);

    // Quotas & Rate Limits
    drogon::Task<drogon::HttpResponsePtr> handleGetQuotas(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateOrgQuota(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateUserQuota(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateServiceQuota(drogon::HttpRequestPtr tsp_req, std::string t_id);

    // Webhooks
    drogon::Task<drogon::HttpResponsePtr> handleListWebhooks(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRegisterWebhook(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteWebhook(drogon::HttpRequestPtr tsp_req, std::string t_id);

private:
    std::string endpoints_cache_;

    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::route_config, 20> kRoutes = {{
        {"/api/v1/admin/status", &AdminApiHandler::handleGetStatus, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/users", &AdminApiHandler::handleGetUsers, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/users/register", &AdminApiHandler::handleRegisterUser, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/automated-services/register", &AdminApiHandler::handleRegisterAutomatedService, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/endpoints", &AdminApiHandler::handleGetEndpoints, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},
        {"/api/v1/admin/metaprobe/sessions", &AdminApiHandler::handleGetMetaProbeSessions, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/automated-services/rotate-token", &AdminApiHandler::handleRotateAutomatedServiceToken, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleListPermissions, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleGrantPermission, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleRevokePermission, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},

        // Roles & RBAC
        {"/api/v1/admin/roles", &AdminApiHandler::handleGetRoles, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/roles", &AdminApiHandler::handleCreateRole, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/roles/assign", &AdminApiHandler::handleAssignRole, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/roles/revoke", &AdminApiHandler::handleRevokeRole, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},

        // Audit Logs
        {"/api/v1/admin/audit-logs", &AdminApiHandler::handleGetAuditLogs, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/audit-logs", &AdminApiHandler::handlePurgeAuditLogs, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},

        // Quotas & Rate Limits
        {"/api/v1/admin/quotas", &AdminApiHandler::handleGetQuotas, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/quotas/organisation", &AdminApiHandler::handleUpdateOrgQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},

        // Webhooks
        {"/api/v1/admin/webhooks", &AdminApiHandler::handleListWebhooks, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/webhooks", &AdminApiHandler::handleRegisterWebhook, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};

    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::item_route_config, 6> kItemRoutes = {{
        {"/api/v1/admin/automated-services/{id}/metadata", &AdminApiHandler::handleUpdateAutomatedServiceMetadata, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/roles/{id}", &AdminApiHandler::handleUpdateRole, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/roles/{id}", &AdminApiHandler::handleDeleteRole, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/quotas/user/{id}", &AdminApiHandler::handleUpdateUserQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/quotas/service/{id}", &AdminApiHandler::handleUpdateServiceQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/webhooks/{id}", &AdminApiHandler::handleDeleteWebhook, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};
};
} // namespace sgrn::datastore::handlers::admin
