#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <json/json.h>

#ifdef DEBUG_ADMIN_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

#define INFO_LOG(msg, ...) SGRN_INFO("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/handlers/admin.hpp>
#include <sgrn/datastore/plugins/postgrest/PostgrestClient.hpp>
#include <sgrn/datastore/services/admin.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/utils/strings.hpp>
#include <orm/models/core/Users.h>

namespace
{
Json::Value parseMetadataValue(const std::string& t_raw) {
    if (t_raw.empty()) {
        return Json::Value(Json::objectValue);
    }
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value metadata(Json::objectValue);
    std::string errors;
    if (!reader->parse(t_raw.c_str(), t_raw.c_str() + t_raw.size(), &metadata, &errors)) {
        return Json::Value(Json::objectValue);
    }
    return metadata;
}
} // namespace

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

Task<HttpResponsePtr> AdminApiHandler::handlePostgrestProxyRequest(HttpRequestPtr tsp_req) {
    auto p_proxy_res = sgrn::datastore::core::getPlugin<::sgrn::datastore::plugins::PostgrestClient>();
    if (p_proxy_res.hasError()) {
        co_return sgrn::createJsonResponse(p_proxy_res);
    }
    co_return co_await p_proxy_res.value()->sendRequest(tsp_req);
}

Task<HttpResponsePtr> AdminApiHandler::handleGetStatus(HttpRequestPtr tsp_req) {
    Json::Value status;
    status["status"] = "operational";
    status["version"] = "1.0.0";
    status["timestamp"] = trantor::Date::now().toFormattedString(false);
    co_return createJsonResponse(status, k200OK);
}

Task<HttpResponsePtr> AdminApiHandler::handleGetUsers(HttpRequestPtr tsp_req) {
    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();
        orm::CoroMapper<drogon_model::sgrn::core::Users> mapper(db);
        auto users = co_await mapper.findAll();

        Json::Value users_json = Json::arrayValue;
        for (const auto& user : users) {
            Json::Value u;
            u["id"] = user.getValueOfId();
            u["email"] = user.getValueOfEmail();
            u["first_name"] = user.getValueOfFirstName();
            u["family_name"] = user.getValueOfFamilyName();
            users_json.append(u);
        }

        co_return createJsonResponse(users_json, k200OK);
    } catch (const std::exception& e) {
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterUser(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    std::string error_msg;
    auto payload_opt = deserializeRegisterUserPayload(json, error_msg);
    if (payload_opt.has_value() == false) {
        co_return createErrorResponse(error_msg, k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();
        // Assuming default role is 'user' if not specified
        std::string role = json->get("role", "user").asString();
        auto result = co_await registerUser(db, *payload_opt, role);

        if (result["success"].asBool()) {
            co_return createJsonResponse(result, k201Created);
        } else {
            co_return createJsonResponse(result, k400BadRequest);
        }
    } catch (const std::exception& e) {
        ERROR_LOG("User registration error: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterAutomatedService(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    if (json == nullptr) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }

    std::string name = sgrn::utils::strings::trim(json->get("name", "").asString());
    std::string kind = sgrn::utils::strings::trim(json->get("kind", "").asString());

    if (name.empty()) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }

    try {
        // Bind automated services to the creator's organisation by default.
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        if (!session.isMember("user") || !session["user"].isMember("organisation") || !session["user"]["organisation"].isString()) {
            co_return createErrorResponse(AdminApiError::InvalidSession);
        }
        const std::string org = session["user"]["organisation"].asString();
        if (org.empty()) {
            co_return createErrorResponse(AdminApiError::InvalidSession);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Create automated service via DB function (generates token + token_secret securely).
        // `kind` is stored in metadata for compatibility with the older admin UI.
        Json::Value meta = json->isMember("metadata") ? (*json)["metadata"] : Json::Value(Json::objectValue);
        if (!kind.empty()) {
            meta["kind"] = kind;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string meta_str = Json::writeString(wb, meta);

        std::optional<int64_t> storage_limit;
        if (json->isMember("storage_limit") && (*json)["storage_limit"].isInt64()) {
            storage_limit = (*json)["storage_limit"].asInt64();
        }

        std::optional<std::string> domain;
        if (json->isMember("domain") && (*json)["domain"].isString()) {
            domain = (*json)["domain"].asString();
        } else if (session.isMember("user") && session["user"].isMember("domain")) {
            // Inherit domain from creator if not specified
            domain = session["user"]["domain"].asString();
        }

        auto res = co_await db->execSqlCoro(
            "SELECT * FROM core.create_automated_service($1, $2::jsonb, $3, $4, $5)", name, meta_str, org, storage_limit, domain);

        if (res.empty())
            co_return createErrorResponse(AdminApiError::DbError);

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Automated service registered successfully";
        resp["automated_service_id"] = res[0]["id"].as<int32_t>();
        resp["name"] = res[0]["name"].as<std::string>();
        resp["token"] = res[0]["token"].as<std::string>();
        resp["token_secret"] = res[0]["token_secret"].as<std::string>();
        resp["organisation"] = res[0]["organisation"].as<std::string>();
        resp["kind"] = kind;

        co_return createJsonResponse(resp, k201Created);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Automated service registration DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ERROR_LOG("Automated service registration error: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleListAutomatedServices(HttpRequestPtr tsp_req) {
    const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
    if (!session.isMember("user") || !session["user"].isMember("organisation") || !session["user"]["organisation"].isString()) {
        co_return createErrorResponse(AdminApiError::InvalidSession);
    }
    const std::string org = session["user"]["organisation"].asString();

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db = db_res.value();

    std::string domain;
    if (session.isMember("user") && session["user"].isMember("domain")) {
        domain = session["user"]["domain"].asString();
    }

    try {
        auto res = co_await db->execSqlCoro("SELECT id, name, token, metadata, status, domain, created_at FROM core.automated_services "
                                            "WHERE organisation = $1 AND (domain = $2 OR $2 IS NULL OR $2 = '') ORDER BY id",
            org, domain);

        Json::Value automated_services = Json::arrayValue;
        for (const auto& row : res) {
            Json::Value svc = Json::objectValue;
            svc["id"] = row["id"].as<int32_t>();
            svc["name"] = row["name"].as<std::string>();
            svc["token"] = row["token"].as<std::string>();
            svc["is_active"] = (row["status"].as<std::string>() == "active");
            svc["status"] = row["status"].as<std::string>();
            svc["domain"] = row["domain"].isNull() ? "" : row["domain"].as<std::string>();
            svc["created_at"] = row["created_at"].as<std::string>();
            const std::string metadata_str = row["metadata"].as<std::string>();
            const Json::Value metadata = parseMetadataValue(metadata_str);
            svc["metadata"] = metadata;
            if (metadata.isObject() && metadata.isMember("kind") && !metadata["kind"].isNull()) {
                svc["kind"] = metadata["kind"];
            }
            automated_services.append(svc);
        }

        co_return createJsonResponse(automated_services, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to list automated services: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateAutomatedServiceMetadata(HttpRequestPtr tsp_req) {
    const std::string id_str = tsp_req->getParameter("id");
    auto json = tsp_req->getJsonObject();
    if (id_str.empty() || !json || !json->isMember("metadata")) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }

    int32_t automated_service_id;
    try {
        automated_service_id = std::stoi(id_str);
    } catch (const std::exception&) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }
    const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
    const std::string org = session["user"]["organisation"].asString();

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError())
        co_return sgrn::createJsonResponse(db_res);
    auto db = db_res.value();

    try {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string meta_str = Json::writeString(wb, (*json)["metadata"]);

        // Update with ORG check to ensure an admin from Org A cannot edit automated services from Org B.
        auto res = co_await db->execSqlCoro(
            "UPDATE core.automated_services SET metadata = metadata || $1::jsonb WHERE id = $2 AND organisation = $3", meta_str,
            automated_service_id, org);

        if (res.affectedRows() == 0) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        co_return createJsonResponse("Automated service metadata updated successfully", k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to update automated service metadata: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRotateAutomatedServiceToken(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    if (!json || !json->isMember("automated_service_id")) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }

    const int32_t automated_service_id = json->get("automated_service_id", 0).asInt();
    if (automated_service_id <= 0) {
        co_return createErrorResponse(AdminApiError::InvalidUserId);
    }

    const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
    if (!session.isMember("user") || !session["user"].isMember("organisation") || !session["user"]["organisation"].isString()) {
        co_return createErrorResponse(AdminApiError::InvalidSession);
    }
    const std::string org = session["user"]["organisation"].asString();

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db = db_res.value();

    try {
        auto owner_res = co_await db->execSqlCoro("SELECT organisation FROM core.automated_services WHERE id = $1", automated_service_id);
        if (owner_res.empty()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        const std::string owner_org = owner_res[0]["organisation"].as<std::string>();
        if (owner_org != org) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto rotation = co_await db->execSqlCoro("SELECT * FROM core.rotate_automated_service_credentials($1, true)", automated_service_id);
        if (rotation.empty()) {
            co_return createErrorResponse(AdminApiError::DbError);
        }

        const auto& updated = rotation[0];
        Json::Value resp = Json::objectValue;
        resp["success"] = true;
        resp["automated_service_id"] = updated["id"].as<int32_t>();
        resp["name"] = updated["name"].as<std::string>();
        resp["token"] = updated["token"].as<std::string>();
        resp["token_secret"] = updated["token_secret"].as<std::string>();
        resp["is_active"] = (updated["status"].as<std::string>() == "active");
        resp["status"] = updated["status"].as<std::string>();
        resp["organisation"] = updated["organisation"].as<std::string>();
        resp["created_at"] = updated["created_at"].as<std::string>();
        const Json::Value metadata = parseMetadataValue(updated["metadata"].as<std::string>());
        resp["metadata"] = metadata;
        if (metadata.isObject() && metadata.isMember("kind") && !metadata["kind"].isNull()) {
            resp["kind"] = metadata["kind"];
        }

        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to rotate credentials: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleGetEndpoints(HttpRequestPtr tsp_req) {
    Json::Value endpoints = Json::arrayValue;
    // Basic discovery - could be expanded to list all registered routes
    endpoints.append("/api/v1/admin/status");
    endpoints.append("/api/v1/admin/users");
    endpoints.append("/api/v1/admin/metaprobe/sessions");
    co_return createJsonResponse(endpoints, k200OK);
}

} // namespace sgrn::datastore::handlers::admin

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
