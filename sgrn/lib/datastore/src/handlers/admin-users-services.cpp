// admin-users-services.cpp — User accounts and automated-service administration.
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

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

Task<HttpResponsePtr> AdminApiHandler::handleGetUsers(HttpRequestPtr tsp_req) {
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);
        const std::string org_param = tsp_req->getParameter("organisation");

        std::string target_org;
        if (!is_platform) {
            if (!org_param.empty() && org_param != session_org) {
                co_return createErrorResponse("Forbidden: cannot list users from another organisation", k403Forbidden, "AdminApi");
            }
            target_org = session_org;
        } else {
            target_org = org_param;
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        auto rows =
            (!target_org.empty())
                ? co_await db->execSqlCoro("SELECT id, email, first_name, family_name, organisation FROM core.users WHERE deleted_at IS "
                                           "NULL AND organisation = $1 ORDER BY id",
                      target_org)
                : co_await db->execSqlCoro(
                      "SELECT id, email, first_name, family_name, organisation FROM core.users WHERE deleted_at IS NULL ORDER BY id");

        Json::Value users_json = Json::arrayValue;
        for (const auto& user : rows) {
            Json::Value u;
            u["id"] = user["id"].as<int32_t>();
            u["email"] = user["email"].as<std::string>();
            u["first_name"] = user["first_name"].as<std::string>();
            u["family_name"] = user["family_name"].as<std::string>();
            u["organisation"] = user["organisation"].as<std::string>();
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

    const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
    const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

    if (!is_platform) {
        if (json->isMember("is_platform_admin") && (*json)["is_platform_admin"].asBool()) {
            co_return createErrorResponse(
                "Forbidden: cannot grant platform admin status (escalation prohibited)", k403Forbidden, "AdminApi");
        }
        std::string req_role = json->get("role", "user").asString();
        if (req_role == "platform_admin") {
            co_return createErrorResponse("Forbidden: cannot grant platform admin role (escalation prohibited)", k403Forbidden, "AdminApi");
        }
        if (!payload_opt->organisation.empty() && payload_opt->organisation != session_org) {
            co_return createErrorResponse("Forbidden: cannot register user in another organisation", k403Forbidden, "AdminApi");
        }
        payload_opt->organisation = session_org;
    } else {
        if (payload_opt->organisation.empty()) {
            payload_opt->organisation = session_org;
        }
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
            const Json::Value& session_json = tsp_req->attributes()->get<Json::Value>("session_json");
            const int32_t actor_id = session_json["user"]["id"].asInt();
            const std::string actor_name = session_json["user"]["email"].asString();
            const std::string org = session_json["user"]["organisation"].asString();
            co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "user.created", "user",
                result.isMember("user_id") ? std::to_string(result["user_id"].asInt()) : payload_opt->email, tsp_req->getPeerAddr().toIp(),
                *json, "success");
            co_return createJsonResponse(result, k201Created);
        } else {
            co_return createJsonResponse(result, k400BadRequest);
        }
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("User registration error: {}", e.what());
        co_return createErrorResponse(std::string("User registration failed: ") + e.what(), k400BadRequest, "AdminApi");
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

        // Sync: insert the new public service token into local RAM sorted vector
        // and broadcast PG NOTIFY so all cluster nodes update their lookup vectors.
        const std::string new_serv_token = res[0]["token"].as<std::string>();
        sgrn::datastore::session::SessionStore::instance().registerServiceToken(new_serv_token);
        co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + new_serv_token);

        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();
        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "service.created", "automated_service",
            std::to_string(res[0]["id"].as<int32_t>()), tsp_req->getPeerAddr().toIp(), *json, "success");

        co_return createJsonResponse(resp, k201Created);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Automated service registration DB error: {}", e.base().what());
        co_return createErrorResponse(std::string("Automated service registration failed: ") + e.base().what(), k400BadRequest, "AdminApi");
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Automated service registration error: {}", e.what());
        co_return createErrorResponse(std::string("Automated service registration failed: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateAutomatedServiceMetadata(HttpRequestPtr tsp_req, std::string t_id) {
    const std::string id_str = std::move(t_id);
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
        ADMIN_ERROR_LOG("Failed to update automated service metadata: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update automated service metadata: ") + e.what(), k400BadRequest, "AdminApi");
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
        auto owner_res =
            co_await db->execSqlCoro("SELECT organisation, token::text FROM core.automated_services WHERE id = $1", automated_service_id);
        if (owner_res.empty()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        const std::string owner_org = owner_res[0]["organisation"].as<std::string>();
        if (owner_org != org) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        const std::string old_tok = owner_res[0]["token"].isNull() ? "" : owner_res[0]["token"].as<std::string>();

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

        // Sync: old public token is replaced — deregister old UUID from sorted vector
        // and register new UUID, broadcasting PG NOTIFY to all cluster nodes.
        if (!old_tok.empty()) {
            sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(old_tok);
            co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + old_tok);
        }
        const std::string new_tok = updated["token"].as<std::string>();
        sgrn::datastore::session::SessionStore::instance().registerServiceToken(new_tok);
        co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + new_tok);

        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to rotate credentials: {}", e.what());
        co_return createErrorResponse(std::string("Failed to rotate credentials: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
