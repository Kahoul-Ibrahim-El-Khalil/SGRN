#pragma once

#include <sgrn/crud/CrudSpec.hpp>
#include <sgrn/crud/CrudViewEngine.hpp>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

namespace sgrn::crud
{

inline drogon::HttpResponsePtr createJsonErrorResponse(
    const std::string& message, drogon::HttpStatusCode code = drogon::k400BadRequest, const std::string& scope = "Client") {
    Json::Value error_obj;
    error_obj["error"] = message;
    error_obj["scope"] = scope;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error_obj);
    resp->setStatusCode(code);
    return resp;
}

template <typename Derived>
class CrudViewHandler : public drogon::HttpController<Derived, false> {
public:
    CrudViewHandler() = default;

    template <size_t N1, size_t N2>
    CrudViewHandler(Derived* /*self*/, const std::array<RouteConfig, N1>& /*routes*/, const std::array<ItemRouteConfig, N2>& /*item_routes*/
    ) {
    }

    CrudViewHandler(Derived* /*self*/, const std::vector<RouteConfig>& /*routes*/, const std::vector<ItemRouteConfig>& /*item_routes*/
    ) {
    }

    drogon::Task<drogon::HttpResponsePtr> handleList(drogon::HttpRequestPtr req) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.read_scope == ReadScope::Org) {
            co_return createJsonErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await CrudViewEngine::executeList(Derived::kSpec, req, std::move(tenant));
    }

    drogon::Task<drogon::HttpResponsePtr> handleGet(drogon::HttpRequestPtr req, std::string id) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.read_scope == ReadScope::Org) {
            co_return createJsonErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await CrudViewEngine::executeGet(Derived::kSpec, req, std::move(tenant), std::move(id));
    }

    drogon::Task<drogon::HttpResponsePtr> handleCreate(drogon::HttpRequestPtr req) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.write_scope == WriteScope::Org) {
            co_return createJsonErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await CrudViewEngine::executeCreate(Derived::kSpec, req, std::move(tenant));
    }

    drogon::Task<drogon::HttpResponsePtr> handleUpdate(drogon::HttpRequestPtr req, std::string id) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.write_scope == WriteScope::Org) {
            co_return createJsonErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await CrudViewEngine::executeUpdate(Derived::kSpec, req, std::move(tenant), std::move(id));
    }

    drogon::Task<drogon::HttpResponsePtr> handleDelete(drogon::HttpRequestPtr req, std::string id) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.delete_scope == DeleteScope::Org) {
            co_return createJsonErrorResponse("Missing organisation identity in session", drogon::k401Unauthorized, "Authentication");
        }
        co_return co_await CrudViewEngine::executeDelete(Derived::kSpec, req, std::move(tenant), std::move(id));
    }

protected:
    static std::string tenantOf(const drogon::HttpRequestPtr& req) {
        try {
            if (req->attributes()->find("session_json")) {
                const Json::Value& session = req->attributes()->get<Json::Value>("session_json");
                if (session.isMember("user") && session["user"].isMember("organisation")) {
                    const Json::Value& org = session["user"]["organisation"];
                    if (org.isString()) {
                        return org.asString();
                    }
                }
            }
            auto tenant_hdr = req->getHeader("X-Tenant-Id");
            if (!tenant_hdr.empty()) {
                return tenant_hdr;
            }
            return {};
        } catch (const std::exception&) {
            return {};
        }
    }
};

} // namespace sgrn::crud
