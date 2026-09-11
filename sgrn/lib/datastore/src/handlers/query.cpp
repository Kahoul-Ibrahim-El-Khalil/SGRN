/* src/sgrn/api/query/handler.cpp */
#include <sgrn/datastore/handlers/query.hpp>

#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <sgrn/datastore/utils/safe_access.hpp>
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
Json::Value modelsToJson(const std::vector<T>& t_models) {
    Json::Value arr(Json::arrayValue);
    for (const auto& item : t_models) {
        Json::Value obj;
        obj["id"] = item.getValueOfId();
        obj["name"] = item.getValueOfName();
        arr.append(std::move(obj));
    }
    return arr;
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryListOfDomains(HttpRequestPtr tsp_http_req) {
    auto org_name = tsp_http_req->getParameter("organisation");
    if (org_name.empty()) {
        co_return sgrn::createJsonErrorResponse("Organisation parameter is required", k400BadRequest);
    }

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createErrorResponse(QueryApiError::DbUnavailable);
    }
    auto client = db_res.value();
    try {
        auto results =
            co_await client->execSqlCoro("SELECT id, name FROM core.domains WHERE organisation = $1 ORDER BY name ASC", org_name);

        Json::Value arr(Json::arrayValue);
        for (const auto& row : results) {
            Json::Value obj;
            obj["id"] = row["id"].as<int32_t>();
            obj["name"] = row["name"].as<std::string>();
            arr.append(std::move(obj));
        }
        co_return HttpResponse::newHttpJsonResponse(arr);
    } catch (const std::exception& e) {
        ERROR_LOG("DB Error: {}", e.what());
        co_return sgrn::createErrorResponse(QueryApiError::DbError);
    }
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryListOfUserStatus(HttpRequestPtr tsp_http_req) {
    // Since core.status table is removed, we return the hardcoded list
    // allowed by the CHECK constraint on core.users.status
    Json::Value arr(Json::arrayValue);

    auto add_status = [&](int t_id, const std::string& t_name) {
        Json::Value obj;
        obj["id"] = t_id;
        obj["name"] = t_name;
        arr.append(std::move(obj));
    };

    add_status(1, "active");
    add_status(2, "suspended");
    add_status(3, "invited");

    co_return HttpResponse::newHttpJsonResponse(arr);
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryListOfOrganisations(HttpRequestPtr tsp_http_req) {
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createErrorResponse(QueryApiError::DbUnavailable);
    }
    auto client = db_res.value();
    try {
        CoroMapper<drogon_model::sgrn::core::Organisations> mapper(client);

        auto results = co_await mapper.orderBy(drogon_model::sgrn::core::Organisations::Cols::_name, SortOrder::ASC).findAll();
        co_return HttpResponse::newHttpJsonResponse(modelsToJson(results));

    } catch (const DrogonDbException& e) {
        ERROR_LOG("DB Error: {}", e.base().what());
        co_return sgrn::createErrorResponse(QueryApiError::DbError);
    }
}

Task<HttpResponsePtr> QueryApiHandler::handleQueryUserInfo(HttpRequestPtr tsp_http_req) {
    auto user_id_opt = getIntParam(tsp_http_req, "userId");
    if (user_id_opt.has_value() == false || user_id_opt.value() == 0) {
        co_return sgrn::createErrorResponse(QueryApiError::InvalidUserId);
    }

    constexpr char query_user_info[] = "SELECT * FROM core.user_details WHERE id = $1";

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError())
        co_return sgrn::createErrorResponse(QueryApiError::DbUnavailable);
    auto client = db_res.value();

    try {
        auto result = co_await client->execSqlCoro(query_user_info, user_id_opt.value());

        if (result.empty()) {
            co_return sgrn::createErrorResponse(QueryApiError::UserNotFound);
        }

        Json::Value row_json;
        auto row = result.front();
        row_json["id"] = row["id"].as<int32_t>();
        row_json["first_name"] = row["first_name"].as<std::string>();
        row_json["family_name"] = row["family_name"].as<std::string>();
        row_json["email"] = row["email"].as<std::string>();
        row_json["organisation"] = row["organisation"].as<std::string>();
        row_json["domain"] = row["domain"].isNull() ? "" : row["domain"].as<std::string>();
        row_json["status"] = row["status"].as<std::string>();

        co_return HttpResponse::newHttpJsonResponse(std::move(row_json));

    } catch (const std::exception& e) {
        ERROR_LOG("DB Error: {}", e.what());
        co_return sgrn::createErrorResponse(QueryApiError::DbError);
    }
}

drogon::Task<drogon::HttpResponsePtr> QueryApiHandler::handleUpdateUserInfo(drogon::HttpRequestPtr tsp_http_req) {
    const auto p_json = tsp_http_req->getJsonObject();
    if (p_json == nullptr) {
        co_return sgrn::createErrorResponse(QueryApiError::InvalidPayload);
    }

    std::string first_name = p_json->get("first_name", "").asString();
    std::string family_name = p_json->get("family_name", "").asString();
    std::string phone_number = p_json->get("phone_number", "").asString();

    if (first_name.empty() || family_name.empty()) {
        co_return sgrn::createErrorResponse(QueryApiError::InvalidPayload);
    }

    // Extract user_id from session (set by UserAuthFilter)
    int32_t user_id = tsp_http_req->attributes()->get<Json::Value>("session_json")["user"]["id"].asInt();

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createErrorResponse(QueryApiError::DbUnavailable);
    }
    auto client = db_res.value();

    try {
        co_await client->execSqlCoro("UPDATE core.users SET first_name = $1, family_name = $2, phone_number = $3 WHERE id = $4", first_name,
            family_name, phone_number, user_id);

        Json::Value response;
        response["message"] = "Profile updated successfully";
        co_return HttpResponse::newHttpJsonResponse(std::move(response));

    } catch (const std::exception& e) {
        ERROR_LOG("Update User Info Error: {}", e.what());
        co_return sgrn::createErrorResponse(QueryApiError::DbError);
    }
}

} // namespace sgrn::datastore::handlers::query

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
