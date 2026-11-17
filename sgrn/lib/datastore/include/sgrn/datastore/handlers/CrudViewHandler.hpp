#pragma once
#include <sgrn/datastore/query/CrudViewBody.hpp>
#include <sgrn/datastore/query/CrudViewBodyBind.hpp>
#include <sgrn/datastore/query/CrudViewEngine.hpp>
#include <sgrn/datastore/query/CrudViewRows.hpp>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>

#include <json/json.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sgrn::datastore::handlers::query
{

// Sinks used by the generated views (which define `Row`) to collect the
// engine's rows and serialize them in the negotiated wire format.
template <typename Row, sgrn::orm::FixedString Query>
class TypedRowsSink final : public sgrn::datastore::query::TypedRowSink {
public:
    void add(const drogon::orm::Row& t_row) override {
        rows_.push_back(sgrn::datastore::query::rowFromOrm<Row, Query>(t_row));
    }

    drogon::HttpResponsePtr makeResponse(const drogon::HttpRequestPtr& tsp_req, drogon::HttpStatusCode t_code) override {
        return sgrn::createTypedResponse(tsp_req, rows_, t_code);
    }

private:
    std::vector<Row> rows_;
};

template <typename Row, sgrn::orm::FixedString Query, sgrn::orm::FixedString PrimaryKey>
class TypedRowSinkOne final : public sgrn::datastore::query::TypedRowSink {
public:
    void add(const drogon::orm::Row& t_row) override {
        row_ = sgrn::datastore::query::rowFromOrm<Row, Query>(t_row);
    }

    std::string primaryKeyText(const drogon::orm::Row& t_row, std::string_view t_field_name) override {
        if (t_field_name != PrimaryKey.view()) {
            throw std::runtime_error("Generated CRUD primary key does not match its row type");
        }
        Row row = sgrn::datastore::query::rowFromOrm<Row, Query>(t_row);
        return sgrn::datastore::query::rowPrimaryKeyText<Row, PrimaryKey>(row);
    }

    drogon::HttpResponsePtr makeResponse(const drogon::HttpRequestPtr& tsp_req, drogon::HttpStatusCode t_code) override {
        return sgrn::createTypedResponse(tsp_req, row_, t_code);
    }

private:
    std::optional<Row> row_;
};

// CRTP base that plugs the generic CRUD engine into the existing
// route-registration system. Generated views derive from this; hand-written
// business-logic endpoints derive from sgrn::IHandler directly.
//
// NOTE on the template argument: the base inherits from
// `IHandler<CrudViewHandler<Derived>>` (not `IHandler<Derived>`). The
// handlers below are members of `CrudViewHandler<Derived>`, so
// `&DerivedView::handleList` has type
// `Task (CrudViewHandler<Derived>::*)(HttpRequestPtr)` — exactly what this
// IHandler specialization's RouteConfig expects. (Had the base been
// `IHandler<Derived>`, the inherited member-pointer would not convert to
// `Task (Derived::*)(...)` and no generated `.gen.hpp` would compile.)
// The derived view only provides `kSpec`/`kFields`/`kRoutes`/`kItemRoutes`
// plus a constructor forwarding to the inherited IHandler constructors.
template <typename Derived>
class CrudViewHandler : public sgrn::IHandler<CrudViewHandler<Derived>> {
public:
    using BaseHandler = sgrn::IHandler<CrudViewHandler<Derived>>;
    using RouteConfig = typename BaseHandler::RouteConfig;
    using ItemRouteConfig = typename BaseHandler::ItemRouteConfig;
    using BaseHandler::BaseHandler;

    drogon::Task<drogon::HttpResponsePtr> handleList(drogon::HttpRequestPtr tsp_req) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Missing organisation identity in session", drogon::k401Unauthorized, "Authentication"});
        }
        if constexpr (requires { typename Derived::Row; }) {
            TypedRowsSink<typename Derived::Row, Derived::kRowProjection> sink;
            co_return co_await sgrn::datastore::query::executeViewList(Derived::kSpec, tsp_req, std::move(tenant), &sink);
        } else {
            co_return co_await sgrn::datastore::query::executeViewList(Derived::kSpec, tsp_req, std::move(tenant));
        }
    }

    drogon::Task<drogon::HttpResponsePtr> handleGet(drogon::HttpRequestPtr tsp_req, std::string t_id) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Missing organisation identity in session", drogon::k401Unauthorized, "Authentication"});
        }
        if constexpr (requires { typename Derived::Row; }) {
            TypedRowSinkOne<typename Derived::Row, Derived::kRowProjection, Derived::kPrimaryKeyName> sink;
            co_return co_await sgrn::datastore::query::executeViewGet(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id), &sink);
        } else {
            co_return co_await sgrn::datastore::query::executeViewGet(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id));
        }
    }

    drogon::Task<drogon::HttpResponsePtr> handleCreate(drogon::HttpRequestPtr tsp_req) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Missing organisation identity in session", drogon::k401Unauthorized, "Authentication"});
        }
        // The request body is decoded straight into the view's concrete body
        // struct (Glaze reflection; JSON, MessagePack or BEVE by content type).
        using Body = typename Derived::CreateBody;
        auto body_res = sgrn::parseTypedRequest<Body>(tsp_req);
        if (body_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{body_res.error(), drogon::k400BadRequest, "Body"});
        }
        const Body& body = body_res.value();
        auto bound = sgrn::datastore::query::bindBody(Derived::kSpec, body, sgrn::datastore::query::WriteOp::Insert);
        if (bound.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{bound.error(), drogon::k400BadRequest, "Body"});
        }
        if constexpr (requires { typename Derived::Row; }) {
            TypedRowSinkOne<typename Derived::Row, Derived::kRowProjection, Derived::kPrimaryKeyName> sink;
            co_return co_await sgrn::datastore::query::executeViewInsertBound(Derived::kSpec, tsp_req, std::move(tenant),
                std::move(bound).value(), sgrn::datastore::query::auditDetailsJson(body), &sink);
        } else {
            co_return co_await sgrn::datastore::query::executeViewInsertBound(
                Derived::kSpec, tsp_req, std::move(tenant), std::move(bound).value(), sgrn::datastore::query::auditDetailsJson(body));
        }
    }

    drogon::Task<drogon::HttpResponsePtr> handleUpdate(drogon::HttpRequestPtr tsp_req, std::string t_id) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Missing organisation identity in session", drogon::k401Unauthorized, "Authentication"});
        }
        using Body = typename Derived::UpdateBody;
        auto body_res = sgrn::parseTypedRequest<Body>(tsp_req);
        if (body_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{body_res.error(), drogon::k400BadRequest, "Body"});
        }
        const Body& body = body_res.value();
        auto bound = sgrn::datastore::query::bindBody(Derived::kSpec, body, sgrn::datastore::query::WriteOp::Update);
        if (bound.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{bound.error(), drogon::k400BadRequest, "Body"});
        }
        if constexpr (requires { typename Derived::Row; }) {
            TypedRowSinkOne<typename Derived::Row, Derived::kRowProjection, Derived::kPrimaryKeyName> sink;
            co_return co_await sgrn::datastore::query::executeViewUpdateBound(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id),
                std::move(bound).value(), sgrn::datastore::query::auditDetailsJson(body), &sink);
        } else {
            co_return co_await sgrn::datastore::query::executeViewUpdateBound(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id),
                std::move(bound).value(), sgrn::datastore::query::auditDetailsJson(body));
        }
    }

    drogon::Task<drogon::HttpResponsePtr> handleDelete(drogon::HttpRequestPtr tsp_req, std::string t_id) {
        std::string tenant = tenantOf(tsp_req);
        if (tenant.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Missing organisation identity in session", drogon::k401Unauthorized, "Authentication"});
        }
        co_return co_await sgrn::datastore::query::executeViewDelete(Derived::kSpec, tsp_req, std::move(tenant), std::move(t_id));
    }

protected:
    static std::string tenantOf(const drogon::HttpRequestPtr& tsp_req) {
        return sgrn::datastore::getSessionOrganisation(tsp_req);
    }
};

} // namespace sgrn::datastore::handlers::query
