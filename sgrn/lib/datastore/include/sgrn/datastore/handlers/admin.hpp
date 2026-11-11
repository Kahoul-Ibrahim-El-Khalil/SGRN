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
    drogon::Task<drogon::HttpResponsePtr> handleSetAutomatedServiceStatus(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleListRoleAssignments(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRevokeSession(drogon::HttpRequestPtr tsp_req, std::string t_id);

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
    drogon::Task<drogon::HttpResponsePtr> handleVerifyAuditLogs(drogon::HttpRequestPtr tsp_req);

    // Quotas & Rate Limits
    drogon::Task<drogon::HttpResponsePtr> handleUpdateOrgQuota(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateUserQuota(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateServiceQuota(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateDomainQuota(drogon::HttpRequestPtr tsp_req, std::string t_id);

    // Organisations (platform admin creates/changes; org admins may list their own)
    drogon::Task<drogon::HttpResponsePtr> handleListOrganisations(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleCreateOrganisation(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateOrganisation(drogon::HttpRequestPtr tsp_req, std::string t_id);
    // Users
    drogon::Task<drogon::HttpResponsePtr> handleSetUserStatus(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteUser(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleResetUserPassword(drogon::HttpRequestPtr tsp_req, std::string t_id);
    // Domains
    drogon::Task<drogon::HttpResponsePtr> handleRenameDomain(drogon::HttpRequestPtr tsp_req, std::string t_id);
    drogon::Task<drogon::HttpResponsePtr> handleMoveDomain(drogon::HttpRequestPtr tsp_req, std::string t_id);
    // Services
    drogon::Task<drogon::HttpResponsePtr> handleDeleteAutomatedService(drogon::HttpRequestPtr tsp_req, std::string t_id);

    // Webhooks
    drogon::Task<drogon::HttpResponsePtr> handleListWebhooks(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRegisterWebhook(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteWebhook(drogon::HttpRequestPtr tsp_req, std::string t_id);

private:
    std::string endpoints_cache_;

    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::RouteConfig, 23> kRoutes = {{
        {"/api/v1/admin/status", &AdminApiHandler::handleGetStatus, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/users", &AdminApiHandler::handleGetUsers, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/users/register", &AdminApiHandler::handleRegisterUser, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/automated-services/register", &AdminApiHandler::handleRegisterAutomatedService, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/endpoints", &AdminApiHandler::handleGetEndpoints, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},
        {"/api/v1/admin/metaprobe/sessions", &AdminApiHandler::handleGetMetaProbeSessions, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/automated-services/rotate-token", &AdminApiHandler::handleRotateAutomatedServiceToken, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleListPermissions, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleGrantPermission, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleRevokePermission, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},

        // Roles & RBAC
        {"/api/v1/admin/roles", &AdminApiHandler::handleGetRoles, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        // Who currently holds which role (org-scoped join over core.user_roles).
        {"/api/v1/admin/roles/assignments", &AdminApiHandler::handleListRoleAssignments, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/roles", &AdminApiHandler::handleCreateRole, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/roles/assign", &AdminApiHandler::handleAssignRole, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/roles/revoke", &AdminApiHandler::handleRevokeRole, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},

        // Audit Logs — read & verify are org-scoped; bulk purge is platform-only (destructive cross-org op)
        {"/api/v1/admin/audit-logs", &AdminApiHandler::handleGetAuditLogs, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/audit-logs/verify", &AdminApiHandler::handleVerifyAuditLogs, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/audit-logs", &AdminApiHandler::handlePurgeAuditLogs, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::PlatformAdminFilter"}},

        // Quotas & Rate Limits
        // GET /api/v1/admin/quotas is owned by StorageAdminHandler::handleListQuotas
        // (single registration; the duplicate here caused F7).
        {"/api/v1/admin/quotas/organisation", &AdminApiHandler::handleUpdateOrgQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},

        // Organisation lifecycle — platform admin creates/changes; both levels list.
        {"/api/v1/admin/organisations", &AdminApiHandler::handleListOrganisations, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/organisations", &AdminApiHandler::handleCreateOrganisation, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::PlatformAdminFilter"}},

        // Webhooks
        {"/api/v1/admin/webhooks", &AdminApiHandler::handleListWebhooks, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/webhooks", &AdminApiHandler::handleRegisterWebhook, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
    }};

    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::ItemRouteConfig, 16> kItemRoutes = {{
        {"/api/v1/admin/automated-services/{id}/metadata", &AdminApiHandler::handleUpdateAutomatedServiceMetadata, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/automated-services/{id}/status", &AdminApiHandler::handleSetAutomatedServiceStatus, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/roles/{id}", &AdminApiHandler::handleUpdateRole, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/roles/{id}", &AdminApiHandler::handleDeleteRole, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/quotas/user/{id}", &AdminApiHandler::handleUpdateUserQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/quotas/service/{id}", &AdminApiHandler::handleUpdateServiceQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/webhooks/{id}", &AdminApiHandler::handleDeleteWebhook, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        // WP5: lifecycle mutations kept behind hand-written routes (scope rules + audit).
        {"/api/v1/admin/organisations/{id}", &AdminApiHandler::handleUpdateOrganisation, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::PlatformAdminFilter"}},
        {"/api/v1/admin/users/{id}/status", &AdminApiHandler::handleSetUserStatus, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        // Per-session revoke (token path segment) — org-scoped, audited.
        {"/api/v1/admin/metaprobe/sessions/{id}", &AdminApiHandler::handleRevokeSession, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/users/{id}", &AdminApiHandler::handleDeleteUser, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/users/{id}/password", &AdminApiHandler::handleResetUserPassword, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/domains/{id}/rename", &AdminApiHandler::handleRenameDomain, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/domains/{id}/move", &AdminApiHandler::handleMoveDomain, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/quotas/domain/{id}", &AdminApiHandler::handleUpdateDomainQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
        {"/api/v1/admin/automated-services/{id}", &AdminApiHandler::handleDeleteAutomatedService, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::OrgAdminFilter"}},
    }};
};
} // namespace sgrn::datastore::handlers::admin
