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

    // GET /api/v1/admin/permissions?user_id=&email=&domain=&organisation=&limit=
    // Joined admin view over core.user_domain_permissions (email + flags).
    // The generic CRUD at /api/v1/user-domain-permissions is tenant-scoped
    // and row-ID addressed; this is the operator surface: identity-joined,
    // searchable, admin-filtered.
    drogon::Task<drogon::HttpResponsePtr> handleListPermissions(drogon::HttpRequestPtr tsp_req);

    // POST /api/v1/admin/permissions
    // Grant (upsert) domain access: {user_id|email, organisation?, domain,
    // allowed_subpath?, can_read?, can_write?, can_delete?}. Organisation
    // defaults to the user's own (cross-org grants are refused); the domain
    // must exist in that organisation (else the FK would 500 — pre-checked
    // for a clean 400). Zero-trust note: granting the FIRST row for a user
    // is what unlocks them; the response says so when that happens.
    drogon::Task<drogon::HttpResponsePtr> handleGrantPermission(drogon::HttpRequestPtr tsp_req);

    // DELETE /api/v1/admin/permissions?id= (or ?user_id=&organisation=&domain=)
    // Revoke. Warns when the user keeps no domain rows afterwards: with
    // zero-trust default-deny that locks them out of storage entirely.
    drogon::Task<drogon::HttpResponsePtr> handleRevokePermission(drogon::HttpRequestPtr tsp_req);

private:
    std::string endpoints_cache_;

    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::route_config, 9> kRoutes = {{
        {"/api/v1/admin/status", &AdminApiHandler::handleGetStatus, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/users", &AdminApiHandler::handleGetUsers, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/users/register", &AdminApiHandler::handleRegisterUser, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/automated-services/register", &AdminApiHandler::handleRegisterAutomatedService, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/endpoints", &AdminApiHandler::handleGetEndpoints, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},
        {"/api/v1/admin/automated-services/rotate-token", &AdminApiHandler::handleRotateAutomatedServiceToken, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleListPermissions, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleGrantPermission, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/permissions", &AdminApiHandler::handleRevokePermission, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};

    // Item route: the `{id}` placeholder is mapped by Drogon onto the
    // handler's second argument (HttpRequest::getParameter() does NOT
    // carry path placeholders for registerHandler routes).
    inline static const std::array<::sgrn::IHandler<AdminApiHandler>::item_route_config, 1> kItemRoutes = {{
        {"/api/v1/admin/automated-services/{id}/metadata", &AdminApiHandler::handleUpdateAutomatedServiceMetadata, {drogon::Patch},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};
};
} // namespace sgrn::datastore::handlers::admin
