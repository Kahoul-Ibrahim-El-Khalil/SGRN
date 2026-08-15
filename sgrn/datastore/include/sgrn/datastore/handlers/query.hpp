/*sgrn/auth/include/auth/query/QueryApiHandler.hpp*/
#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/types/Column.hpp>
#include <sgrn/utils/jsoncpp.hpp>
#include <array>
#include <regex>

namespace sgrn::datastore::handlers::query
{

class QueryApiHandler : public sgrn::IHandler<QueryApiHandler> {
    static constexpr std::array<Column, 2> kIdNameColumns = {{{"id", Column::Type::INT32}, {"name", Column::Type::STRING}}};

public:
    QueryApiHandler()
        : IHandler(this, kRoutes) {
    }

    // Coroutine Signatures: Return Task<HttpResponsePtr>
    drogon::Task<drogon::HttpResponsePtr> handleQueryListOfDomains(drogon::HttpRequestPtr tsp_http_req);
    drogon::Task<drogon::HttpResponsePtr> handleQueryListOfUserStatus(drogon::HttpRequestPtr tsp_http_req);
    drogon::Task<drogon::HttpResponsePtr> handleQueryListOfOrganisations(drogon::HttpRequestPtr tsp_http_req);
    drogon::Task<drogon::HttpResponsePtr> handleQueryUserInfo(drogon::HttpRequestPtr tsp_http_req);
    drogon::Task<drogon::HttpResponsePtr> handleUpdateUserInfo(drogon::HttpRequestPtr tsp_http_req);

    // Columns helpers
    static constexpr const std::array<Column, 2>& organisationColumns() noexcept {
        return kIdNameColumns;
    }

    static constexpr const std::array<Column, 2>& statusColumns() noexcept {
        return kIdNameColumns;
    }

    static constexpr const std::array<Column, 2>& domainColumns() noexcept {
        return kIdNameColumns;
    }

private:
    inline static const std::array<sgrn::IHandler<QueryApiHandler>::route_config, 5> kRoutes = {
        {{"/api/v1/query/organisations", &QueryApiHandler::handleQueryListOfOrganisations, {drogon::Get}, {}},
            {"/api/v1/query/domains", &QueryApiHandler::handleQueryListOfDomains, {drogon::Get}, {}},

            {"/api/v1/query/statuses", &QueryApiHandler::handleQueryListOfUserStatus, {drogon::Get}, {}},
            {"/api/v1/query/user/info", &QueryApiHandler::handleQueryUserInfo, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},
            {"/api/v1/query/user/info", &QueryApiHandler::handleUpdateUserInfo, {drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}}}};
};

} // namespace sgrn::datastore::handlers::query
