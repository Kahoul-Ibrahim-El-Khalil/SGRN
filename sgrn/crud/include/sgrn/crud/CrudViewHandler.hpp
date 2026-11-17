#pragma once

#include <sgrn/crud/CrudSpec.hpp>
#include <sgrn/crud/CrudViewEngine.hpp>
#include <sgrn/respond.hpp>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>

#include <memory>
#include <string>
#include <vector>

namespace sgrn::crud
{

struct CrudErrorResponse {
    std::string error;
    std::string scope;
};

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
            co_return ::createTypedResponse(
                CrudErrorResponse{"Missing organisation identity in session", "Authentication"}, drogon::k401Unauthorized);
        }
        co_return co_await CrudViewEngine::executeList(Derived::kSpec, req, std::move(tenant), responseEncoder());
    }

    drogon::Task<drogon::HttpResponsePtr> handleGet(drogon::HttpRequestPtr req, std::string id) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.read_scope == ReadScope::Org) {
            co_return ::createTypedResponse(
                CrudErrorResponse{"Missing organisation identity in session", "Authentication"}, drogon::k401Unauthorized);
        }
        co_return co_await CrudViewEngine::executeGet(Derived::kSpec, req, std::move(tenant), std::move(id), responseEncoder());
    }

    drogon::Task<drogon::HttpResponsePtr> handleCreate(drogon::HttpRequestPtr req) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.write_scope == WriteScope::Org) {
            co_return ::createTypedResponse(
                CrudErrorResponse{"Missing organisation identity in session", "Authentication"}, drogon::k401Unauthorized);
        }
        if constexpr (requires { typename Derived::CreateBody; }) {
            auto body = parseRequestBody<typename Derived::CreateBody>(req);
            if (!body) {
                co_return ::createTypedResponse(CrudErrorResponse{"Missing or invalid JSON body", "Body"}, drogon::k400BadRequest);
            }
            std::vector<BoundField> bound;
            if (auto error = bindBody(Derived::kSpec, *body, bound)) {
                co_return ::createTypedResponse(CrudErrorResponse{std::move(*error), "Body"}, drogon::k400BadRequest);
            }
            co_return co_await CrudViewEngine::executeCreate(Derived::kSpec, req, std::move(tenant), responseEncoder(), &bound);
        } else {
            co_return co_await CrudViewEngine::executeCreate(Derived::kSpec, req, std::move(tenant), responseEncoder());
        }
    }

    drogon::Task<drogon::HttpResponsePtr> handleUpdate(drogon::HttpRequestPtr req, std::string id) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.write_scope == WriteScope::Org) {
            co_return ::createTypedResponse(
                CrudErrorResponse{"Missing organisation identity in session", "Authentication"}, drogon::k401Unauthorized);
        }
        if constexpr (requires { typename Derived::UpdateBody; }) {
            auto body = parseRequestBody<typename Derived::UpdateBody>(req);
            if (!body) {
                co_return ::createTypedResponse(CrudErrorResponse{"Missing or invalid JSON body", "Body"}, drogon::k400BadRequest);
            }
            std::vector<BoundField> bound;
            if (auto error = bindBody(Derived::kSpec, *body, bound)) {
                co_return ::createTypedResponse(CrudErrorResponse{std::move(*error), "Body"}, drogon::k400BadRequest);
            }
            co_return co_await CrudViewEngine::executeUpdate(
                Derived::kSpec, req, std::move(tenant), std::move(id), responseEncoder(), &bound);
        } else {
            co_return co_await CrudViewEngine::executeUpdate(Derived::kSpec, req, std::move(tenant), std::move(id), responseEncoder());
        }
    }

    drogon::Task<drogon::HttpResponsePtr> handleDelete(drogon::HttpRequestPtr req, std::string id) {
        std::string tenant = tenantOf(req);
        if (tenant.empty() && Derived::kSpec.policy.delete_scope == DeleteScope::Org) {
            co_return ::createTypedResponse(
                CrudErrorResponse{"Missing organisation identity in session", "Authentication"}, drogon::k401Unauthorized);
        }
        co_return co_await CrudViewEngine::executeDelete(Derived::kSpec, req, std::move(tenant), std::move(id));
    }

protected:
    template <typename Body>
    static std::optional<Body> parseRequestBody(const drogon::HttpRequestPtr& req) {
        const std::string_view raw = req->body();
        if (raw.empty()) {
            return std::nullopt;
        }
        Body body{};
        if (glz::read_json(body, raw)) {
            return std::nullopt;
        }
        return body;
    }

    static const RowResponseEncoder* responseEncoder() {
        if constexpr (requires { typename Derived::ResponseRow; }) {
            static const TypedRowResponseEncoder<typename Derived::ResponseRow> encoder;
            return &encoder;
        }
        return nullptr;
    }

    static std::string tenantOf(const drogon::HttpRequestPtr& req) {
        if (req->attributes()->find("tenant_id")) {
            return req->attributes()->get<std::string>("tenant_id");
        }
        auto tenant_hdr = req->getHeader("X-Tenant-Id");
        return tenant_hdr.empty() ? std::string{} : tenant_hdr;
    }
};

} // namespace sgrn::crud
