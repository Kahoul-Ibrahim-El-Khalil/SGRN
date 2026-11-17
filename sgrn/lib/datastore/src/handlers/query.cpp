/* src/sgrn/api/query/handler.cpp */
#include <sgrn/datastore/handlers/query.hpp>

#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/wire/wire.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/jsoncpp.hpp>
#include <orm/models/core/Organisations.h>

#ifdef DEBUG_QUERY_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("QueryHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("QueryHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("QueryHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("QueryHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::sgrn;

namespace sgrn::datastore::handlers::query
{

namespace detail
{
struct UpdateUserInfoRequest {
    std::string first_name;
    std::string family_name;
    std::string phone_number;
};

using IdNameResponse = ::sgrn::datastore::common::IdName;

using UserInfoResponse = ::sgrn::datastore::auth::UserInfo;
} // namespace detail
using namespace detail;

static std::optional<int32_t> getIntParam(HttpRequestPtr tsp_http_req, const std::string& t_param_name) {
    const std::string& val = tsp_http_req->getParameter(t_param_name);
    if (val.empty()) {
        return std::nullopt;
    }
    try {
        return std::stoi(val);
    } catch (const std::invalid_argument& e) {
        SGRN_WARN_LOG("QueryHandler", "Invalid integer for {}: {}", t_param_name, e.what());
        return std::nullopt;
    } catch (const std::out_of_range& e) {
        SGRN_WARN_LOG("QueryHandler", "Integer out of range for {}: {}", t_param_name, e.what());
        return std::nullopt;
    }
}

template <typename T>
std::vector<IdNameResponse> modelsToResponse(const std::vector<T>& t_models) {
    std::vector<IdNameResponse> arr;
    arr.reserve(t_models.size());
    for (const auto& item : t_models) {
        arr.push_back({item.getValueOfId(), item.getValueOfName()});
    }
    return arr;
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryListOfUserStatus(HttpRequestPtr tsp_http_req) {
    // Since core.status table is removed, we return the hardcoded list
    // allowed by the CHECK constraint on core.users.status
    std::vector<IdNameResponse> statuses{{1, "active"}, {2, "suspended"}, {3, "invited"}};
    co_return createTypedResponse(tsp_http_req, statuses, k200OK);
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryListOfOrganisations(HttpRequestPtr tsp_http_req) {
    auto db_res = sgrn::datastore::core::getDbClient();
    CO_RETURN_IF(db_res.hasError(), sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::DbUnavailable}));
    auto client = db_res.value();
    try {
        CoroMapper<drogon_model::sgrn::core::Organisations> mapper(client);

        auto results = co_await mapper.orderBy(drogon_model::sgrn::core::Organisations::Cols::_name, SortOrder::ASC).findAll();
        co_return createTypedResponse(tsp_http_req, modelsToResponse(results), k200OK);

    } catch (const DrogonDbException& e) {
        ERROR_LOG("DB Error: {}", e.base().what());
        co_return sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::DbError});
    }
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryUserInfo(HttpRequestPtr tsp_http_req) {
    auto user_id_opt = getIntParam(tsp_http_req, "userId");
    if (user_id_opt.has_value() == false || user_id_opt.value() == 0) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::InvalidUserId});
    }

    constexpr char query_user_info[] = "SELECT * FROM core.user_details WHERE id = $1";

    auto db_res = sgrn::datastore::core::getDbClient();
    CO_RETURN_IF(db_res.hasError(), sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::DbUnavailable}));
    auto client = db_res.value();

    try {
        auto result = co_await client->execSqlCoro(query_user_info, user_id_opt.value());

        auto response_opt = sgrn::orm::fromSingleResultByName<UserInfoResponse>(result);
        if (!response_opt) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::UserNotFound});
        }

        co_return createTypedResponse(tsp_http_req, *response_opt, k200OK);

    } catch (const std::exception& e) {
        ERROR_LOG("DB Error: {}", e.what());
        co_return sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::DbError});
    }
}

drogon::Task<drogon::HttpResponsePtr> QueryApiHandler::handleUpdateUserInfo(drogon::HttpRequestPtr tsp_http_req) {
    auto req_res = parseTypedRequest<UpdateUserInfoRequest>(tsp_http_req);
    if (req_res.hasError() || req_res->first_name.empty() || req_res->family_name.empty()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::InvalidPayload});
    }
    const auto& req = req_res.value();
    const std::string& first_name = req.first_name;
    const std::string& family_name = req.family_name;
    const std::string& phone_number = req.phone_number;

    // Extract user_id from session (set by UserAuthFilter)
    int32_t user_id = 0;
    if (tsp_http_req->attributes()->find("auth_context")) {
        user_id = tsp_http_req->attributes()->get<sgrn::datastore::filters::AuthContext>("auth_context").user.id;
    } else if (tsp_http_req->attributes()->find("session_claims")) {
        user_id = tsp_http_req->attributes()->get<sgrn::datastore::auth::SessionClaims>("session_claims").user.id;
    }

    auto db_res = sgrn::datastore::core::getDbClient();
    CO_RETURN_IF(db_res.hasError(), sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::DbUnavailable}));
    auto client = db_res.value();

    try {
        co_await client->execSqlCoro("UPDATE core.users SET first_name = $1, family_name = $2, phone_number = $3 WHERE id = $4", first_name,
            family_name, phone_number, user_id);

        co_return createTypedResponse(tsp_http_req, MessageResponse{"Profile updated successfully"}, k200OK);

    } catch (const std::exception& e) {
        ERROR_LOG("Update User Info Error: {}", e.what());
        co_return sgrn::createTypedResponse(BackendErrorResponse{QueryApiError::DbError});
    }
}

} // namespace sgrn::datastore::handlers::query

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
