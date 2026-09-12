#pragma once
#include <sgrn/datastore/query/CrudViewEngine.hpp>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/datastore/utils/respond.hpp>

#include <json/json.h>
#include <string>

namespace sgrn::datastore::handlers::query
{

// CRTP base that plugs the generic CRUD engine into the existing
// route-registration system. Generated views derive from this; hand-written
// business-logic endpoints derive from sgrn::IHandler directly.
//
// NOTE on the template argument: the base inherits from
// `IHandler<CrudViewHandler<Derived>>` (not `IHandler<Derived>`). The
// handlers below are members of `CrudViewHandler<Derived>`, so
// `&DerivedView::handleList` has type
// `Task (CrudViewHandler<Derived>::*)(HttpRequestPtr)` — exactly what this
// IHandler specialization's route_config expects. (Had the base been
// `IHandler<Derived>`, the inherited member-pointer would not convert to
// `Task (Derived::*)(...)` and no generated `.gen.hpp` would compile.)
// The derived view only provides `kSpec`/`kFields`/`kRoutes`/`kItemRoutes`
// plus a constructor forwarding to the inherited IHandler constructors.
template <typename Derived>
class CrudViewHandler : public sgrn::IHandler<CrudViewHandler<Derived>> {
public:
    using BaseHandler = sgrn::IHandler<CrudViewHandler<Derived>>;
    using RouteConfig = typename BaseHandler::route_config;
    using ItemRouteConfig = typename BaseHandler::item_route_config;
    using BaseHandler::BaseHandler;

    drogon::Task<drogon::HttpResponsePtr> handleList(drogon::HttpRequestPtr tsp_req) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await sgrn::datastore::query::executeViewList(Derived::kSpec, tsp_req, std::move(tenant));
    }

    drogon::Task<drogon::HttpResponsePtr> handleGet(drogon::HttpRequestPtr tsp_req, std::string t_id) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await sgrn::datastore::query::executeViewGet(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id));
    }

    drogon::Task<drogon::HttpResponsePtr> handleCreate(drogon::HttpRequestPtr tsp_req) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await sgrn::datastore::query::executeViewInsert(Derived::kSpec, tsp_req, std::move(tenant));
    }

    drogon::Task<drogon::HttpResponsePtr> handleUpdate(drogon::HttpRequestPtr tsp_req, std::string t_id) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await sgrn::datastore::query::executeViewUpdate(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id));
    }

    drogon::Task<drogon::HttpResponsePtr> handleDelete(drogon::HttpRequestPtr tsp_req, std::string t_id) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await sgrn::datastore::query::executeViewDelete(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id));
    }

protected:
    static std::string tenantOf(const drogon::HttpRequestPtr& tsp_req) {
        try {
            if (!tsp_req->attributes()->find("session_json")) {
                return {};
            }
            const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
            if (!session.isMember("user") || !session["user"].isMember("organisation")) {
                return {};
            }
            const Json::Value& org = session["user"]["organisation"];
            if (!org.isString()) {
                return {};
            }
            return org.asString();
        } catch (const std::exception&) {
            return {};
        }
    }
};

} // namespace sgrn::datastore::handlers::query
