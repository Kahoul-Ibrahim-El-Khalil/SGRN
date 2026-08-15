#pragma once
#include <drogon/HttpAppFramework.h>
#include <fmt/core.h>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <array>
#include <string>

namespace sgrn::datastore::handlers::auth
{

// The API Handler using enum-based routes
class AuthApiHandler : public ::sgrn::IHandler<AuthApiHandler> {
public:
    AuthApiHandler()
        : ::sgrn::IHandler<AuthApiHandler>(this, kRoutes) {
    }

    // Drogon Task-based handlers (Coroutine support)
    drogon::Task<drogon::HttpResponsePtr> handleSignIn(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAutomatedServiceSignIn(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleSignOut(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAutomatedServiceSignOut(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdatePassword(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetAutomatedServiceSession(drogon::HttpRequestPtr tsp_req);

private:
    inline static const std::array<typename sgrn::IHandler<AuthApiHandler>::route_config, 6> kRoutes{
        {{"/api/v1/auth/user/signin", &AuthApiHandler::handleSignIn, {drogon::Post}, {}},
            {"/api/v1/auth/automated-service/signin", &AuthApiHandler::handleAutomatedServiceSignIn, {drogon::Post}, {}},
            {"/api/v1/auth/automated-service/signout", &AuthApiHandler::handleAutomatedServiceSignOut, {drogon::Post},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},
            {"/api/v1/auth/automated-service/session", &AuthApiHandler::handleGetAutomatedServiceSession, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},
            {"/api/v1/auth/user/signout", &AuthApiHandler::handleSignOut, {drogon::Post}, {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/auth/user/password", &AuthApiHandler::handleUpdatePassword, {drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}}}};
};

} // namespace sgrn::datastore::handlers::auth
