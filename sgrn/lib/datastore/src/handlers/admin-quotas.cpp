// admin-quotas.cpp — Organisation, user and service quotas..
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

Task<HttpResponsePtr> AdminApiHandler::handleGetQuotas(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // Org level limits
        auto org_res = co_await db->execSqlCoro(
            "SELECT total_virtual_size, total_real_size, storage_limit, total_entry_count, entry_count_limit, rate_limit_rpm "
            "FROM core.organisations WHERE name = $1",
            org);

        Json::Value org_json;
        if (!org_res.empty()) {
            const auto& r = org_res[0];
            org_json["total_virtual_size"] = r["total_virtual_size"].as<int64_t>();
            org_json["total_real_size"] = r["total_real_size"].as<int64_t>();
            org_json["storage_limit"] =
                r["storage_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(r["storage_limit"].as<int64_t>());
            org_json["total_entry_count"] = r["total_entry_count"].as<int64_t>();
            org_json["entry_count_limit"] =
                r["entry_count_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(r["entry_count_limit"].as<int64_t>());
            org_json["rate_limit_rpm"] =
                r["rate_limit_rpm"].isNull() ? Json::Value(Json::nullValue) : Json::Value(r["rate_limit_rpm"].as<int32_t>());
        }

        // Users quota list
        auto users_res = co_await db->execSqlCoro(
            "SELECT id, email, total_virtual_size, total_real_size, storage_limit, total_entry_count, entry_count_limit, rate_limit_rpm, "
            "max_file_size_mb, preferred_chunk_size_mb, rate_limit_upload_rpm "
            "FROM core.users WHERE organisation = $1 AND deleted_at IS NULL ORDER BY email",
            org);

        Json::Value users_arr = Json::arrayValue;
        for (const auto& u : users_res) {
            Json::Value u_j;
            u_j["id"] = u["id"].as<int32_t>();
            u_j["email"] = u["email"].as<std::string>();
            u_j["total_virtual_size"] = u["total_virtual_size"].as<int64_t>();
            u_j["total_real_size"] = u["total_real_size"].as<int64_t>();
            u_j["storage_limit"] =
                u["storage_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(u["storage_limit"].as<int64_t>());
            u_j["total_entry_count"] = u["total_entry_count"].as<int64_t>();
            u_j["entry_count_limit"] =
                u["entry_count_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(u["entry_count_limit"].as<int64_t>());
            u_j["rate_limit_rpm"] =
                u["rate_limit_rpm"].isNull() ? Json::Value(Json::nullValue) : Json::Value(u["rate_limit_rpm"].as<int32_t>());
            u_j["max_file_size_mb"] =
                u["max_file_size_mb"].isNull() ? Json::Value(Json::nullValue) : Json::Value(u["max_file_size_mb"].as<int64_t>());
            u_j["preferred_chunk_size_mb"] = u["preferred_chunk_size_mb"].isNull()
                                                 ? Json::Value(Json::nullValue)
                                                 : Json::Value(u["preferred_chunk_size_mb"].as<int32_t>());
            u_j["rate_limit_upload_rpm"] =
                u["rate_limit_upload_rpm"].isNull() ? Json::Value(Json::nullValue) : Json::Value(u["rate_limit_upload_rpm"].as<int32_t>());
            users_arr.append(u_j);
        }

        // Automated services quota list
        auto serv_res =
            co_await db->execSqlCoro("SELECT id, name, token::text, total_virtual_size, total_real_size, storage_limit, total_entry_count, "
                                     "entry_count_limit, rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb, rate_limit_upload_rpm "
                                     "FROM core.automated_services WHERE organisation = $1 AND deleted_at IS NULL ORDER BY name",
                org);

        Json::Value serv_arr = Json::arrayValue;
        for (const auto& s : serv_res) {
            Json::Value s_j;
            s_j["id"] = s["id"].as<int32_t>();
            s_j["name"] = s["name"].as<std::string>();
            s_j["token"] = s["token"].as<std::string>();
            s_j["total_virtual_size"] = s["total_virtual_size"].as<int64_t>();
            s_j["total_real_size"] = s["total_real_size"].as<int64_t>();
            s_j["storage_limit"] =
                s["storage_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(s["storage_limit"].as<int64_t>());
            s_j["total_entry_count"] = s["total_entry_count"].as<int64_t>();
            s_j["entry_count_limit"] =
                s["entry_count_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(s["entry_count_limit"].as<int64_t>());
            s_j["rate_limit_rpm"] =
                s["rate_limit_rpm"].isNull() ? Json::Value(Json::nullValue) : Json::Value(s["rate_limit_rpm"].as<int32_t>());
            s_j["max_file_size_mb"] =
                s["max_file_size_mb"].isNull() ? Json::Value(Json::nullValue) : Json::Value(s["max_file_size_mb"].as<int64_t>());
            s_j["preferred_chunk_size_mb"] = s["preferred_chunk_size_mb"].isNull()
                                                 ? Json::Value(Json::nullValue)
                                                 : Json::Value(s["preferred_chunk_size_mb"].as<int32_t>());
            s_j["rate_limit_upload_rpm"] =
                s["rate_limit_upload_rpm"].isNull() ? Json::Value(Json::nullValue) : Json::Value(s["rate_limit_upload_rpm"].as<int32_t>());
            serv_arr.append(s_j);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["organisation"] = org_json;
        resp["users"] = users_arr;
        resp["automated_services"] = serv_arr;
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to get quotas: {}", e.what());
        co_return createErrorResponse(std::string("Failed to get quotas: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateOrgQuota(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json)
            co_return createErrorResponse(AdminApiError::InvalidPayload);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        std::optional<int64_t> storage_limit;
        std::optional<int64_t> entry_count_limit;
        std::optional<int32_t> rate_limit_rpm;

        if (json->isMember("storage_limit") && (*json)["storage_limit"].isInt64())
            storage_limit = (*json)["storage_limit"].asInt64();
        if (json->isMember("entry_count_limit") && (*json)["entry_count_limit"].isInt64())
            entry_count_limit = (*json)["entry_count_limit"].asInt64();
        if (json->isMember("rate_limit_rpm") && (*json)["rate_limit_rpm"].isInt())
            rate_limit_rpm = (*json)["rate_limit_rpm"].asInt();

        co_await db->execSqlCoro(
            "UPDATE core.organisations SET storage_limit = $2, entry_count_limit = $3, rate_limit_rpm = $4 WHERE name = $1", org,
            storage_limit, entry_count_limit, rate_limit_rpm);

        co_await sgrn::datastore::audit::AuditLogger::log(
            db, org, "user", actor_id, actor_name, "quota.updated", "organisation", org, tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Organisation quota and limits updated successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to update org quota: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update org quota: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateUserQuota(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const int32_t user_id = std::stoi(t_id);
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json)
            co_return createErrorResponse(AdminApiError::InvalidPayload);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        std::optional<int64_t> storage_limit;
        std::optional<int64_t> entry_count_limit;
        std::optional<int32_t> rate_limit_rpm;
        std::optional<int64_t> max_file_size_mb;
        std::optional<int32_t> preferred_chunk_size_mb;
        std::optional<int32_t> rate_limit_upload_rpm;

        if (json->isMember("storage_limit") && !(*json)["storage_limit"].isNull() && (*json)["storage_limit"].isInt64())
            storage_limit = (*json)["storage_limit"].asInt64();
        if (json->isMember("entry_count_limit") && !(*json)["entry_count_limit"].isNull() && (*json)["entry_count_limit"].isInt64())
            entry_count_limit = (*json)["entry_count_limit"].asInt64();
        if (json->isMember("rate_limit_rpm") && !(*json)["rate_limit_rpm"].isNull() && (*json)["rate_limit_rpm"].isInt())
            rate_limit_rpm = (*json)["rate_limit_rpm"].asInt();
        if (json->isMember("max_file_size_mb") && !(*json)["max_file_size_mb"].isNull() && (*json)["max_file_size_mb"].isInt64())
            max_file_size_mb = (*json)["max_file_size_mb"].asInt64();
        if (json->isMember("preferred_chunk_size_mb") && !(*json)["preferred_chunk_size_mb"].isNull() &&
            (*json)["preferred_chunk_size_mb"].isInt())
            preferred_chunk_size_mb = (*json)["preferred_chunk_size_mb"].asInt();
        if (json->isMember("rate_limit_upload_rpm") && !(*json)["rate_limit_upload_rpm"].isNull() &&
            (*json)["rate_limit_upload_rpm"].isInt())
            rate_limit_upload_rpm = (*json)["rate_limit_upload_rpm"].asInt();

        auto res = co_await db->execSqlCoro("UPDATE core.users SET storage_limit = $3, entry_count_limit = $4, rate_limit_rpm = $5, "
                                            "max_file_size_mb = $6, preferred_chunk_size_mb = $7, rate_limit_upload_rpm = $8 WHERE "
                                            "organisation = $1 AND id = $2 AND deleted_at IS NULL",
            org, user_id, storage_limit, entry_count_limit, rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb,
            rate_limit_upload_rpm);

        if (res.affectedRows() == 0) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "quota.updated", "user",
            std::to_string(user_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "User quota and limits updated successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to update user quota: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update user quota: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateServiceQuota(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const int32_t service_id = std::stoi(t_id);
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json)
            co_return createErrorResponse(AdminApiError::InvalidPayload);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        std::optional<int64_t> storage_limit;
        std::optional<int64_t> entry_count_limit;
        std::optional<int32_t> rate_limit_rpm;
        std::optional<int64_t> max_file_size_mb;
        std::optional<int32_t> preferred_chunk_size_mb;
        std::optional<int32_t> rate_limit_upload_rpm;

        if (json->isMember("storage_limit") && !(*json)["storage_limit"].isNull() && (*json)["storage_limit"].isInt64())
            storage_limit = (*json)["storage_limit"].asInt64();
        if (json->isMember("entry_count_limit") && !(*json)["entry_count_limit"].isNull() && (*json)["entry_count_limit"].isInt64())
            entry_count_limit = (*json)["entry_count_limit"].asInt64();
        if (json->isMember("rate_limit_rpm") && !(*json)["rate_limit_rpm"].isNull() && (*json)["rate_limit_rpm"].isInt())
            rate_limit_rpm = (*json)["rate_limit_rpm"].asInt();
        if (json->isMember("max_file_size_mb") && !(*json)["max_file_size_mb"].isNull() && (*json)["max_file_size_mb"].isInt64())
            max_file_size_mb = (*json)["max_file_size_mb"].asInt64();
        if (json->isMember("preferred_chunk_size_mb") && !(*json)["preferred_chunk_size_mb"].isNull() &&
            (*json)["preferred_chunk_size_mb"].isInt())
            preferred_chunk_size_mb = (*json)["preferred_chunk_size_mb"].asInt();
        if (json->isMember("rate_limit_upload_rpm") && !(*json)["rate_limit_upload_rpm"].isNull() &&
            (*json)["rate_limit_upload_rpm"].isInt())
            rate_limit_upload_rpm = (*json)["rate_limit_upload_rpm"].asInt();

        auto res =
            co_await db->execSqlCoro("UPDATE core.automated_services SET storage_limit = $3, entry_count_limit = $4, rate_limit_rpm = $5, "
                                     "max_file_size_mb = $6, preferred_chunk_size_mb = $7, rate_limit_upload_rpm = $8 WHERE "
                                     "organisation = $1 AND id = $2 AND deleted_at IS NULL",
                org, service_id, storage_limit, entry_count_limit, rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb,
                rate_limit_upload_rpm);

        if (res.affectedRows() == 0) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "quota.updated", "automated_service",
            std::to_string(service_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Automated service quota and limits updated successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to update service quota: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update service quota: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
