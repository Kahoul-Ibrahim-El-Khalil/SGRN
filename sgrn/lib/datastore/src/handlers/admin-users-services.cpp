// admin-users-services.cpp — User accounts and automated-service administration.
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace
{
struct UserListEntry {
    int32_t id{0};
    std::string email;
    std::string first_name;
    std::string family_name;
    std::string organisation;
};

struct ServiceRegistrationResponse {
    bool success{true};
    std::string message;
    int32_t automated_service_id{0};
    std::string name;
    std::string token;
    std::string token_secret;
    std::string organisation;
    std::string kind;
};

struct ServiceRotationResponse {
    bool success{true};
    int32_t automated_service_id{0};
    std::string name;
    std::string token;
    std::string token_secret;
    bool is_active{false};
    std::string status;
    std::string organisation;
    std::string created_at;
    sgrn::json::RawJson metadata{"{}"};
    std::optional<sgrn::json::RawJson> kind;
};

struct UserStatusResponse {
    bool success{true};
    std::string status;
};
struct DeletedUserResponse {
    bool success{true};
    int32_t deleted_user_id{0};
};
struct SuccessResponse {
    bool success{true};
};
struct ServiceStatusResponse {
    bool success{true};
    int32_t automated_service_id{0};
    std::string status;
};
struct DeletedServiceResponse {
    bool success{true};
    int32_t deleted_service_id{0};
};

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

struct UserTarget {
    int32_t id{};
    std::string organisation;
    std::string email;
    bool is_platform_admin{false};
    std::string status;
};

// Resolves an active (non-soft-deleted) user. Returns nullopt for "missing"
// AND "belongs to another organisation" so callers answer identically (no
// tenant enumeration).
drogon::Task<std::optional<UserTarget>> loadUserTarget(
    const drogon::orm::DbClientPtr& t_db, int32_t t_id, const std::string& t_caller_org, bool t_is_platform) {
    auto rows = co_await t_db->execSqlCoro(
        "SELECT id, organisation, email, is_platform_admin, status FROM core.users WHERE id = $1 AND deleted_at IS NULL", t_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    UserTarget target;
    target.id = rows[0]["id"].as<int32_t>();
    target.organisation = rows[0]["organisation"].as<std::string>();
    target.email = rows[0]["email"].as<std::string>();
    target.is_platform_admin = rows[0]["is_platform_admin"].as<bool>();
    target.status = rows[0]["status"].as<std::string>();
    if (!t_is_platform && target.organisation != t_caller_org) {
        co_return std::nullopt;
    }
    co_return target;
}

// True when the user holds an organisation-scope admin assignment and nobody
// else in that organisation can still administer it.
drogon::Task<bool> isLastOrgAdmin(const drogon::orm::DbClientPtr& t_db, int32_t t_user_id) {
    auto rows = co_await t_db->execSqlCoro(
        "SELECT EXISTS (SELECT 1 FROM core.admin_assignments WHERE user_id = $1 AND scope = 'organisation') AS is_admin, "
        "(SELECT count(*) FROM core.admin_assignments a JOIN core.users u ON u.id = a.user_id "
        " WHERE a.scope = 'organisation' AND a.organisation = (SELECT organisation FROM core.users WHERE id = $1) "
        "   AND u.status = 'active' AND u.deleted_at IS NULL AND u.id <> $1) AS other_admins",
        t_user_id);
    if (rows.empty()) {
        co_return false;
    }
    co_return rows[0]["is_admin"].as<bool>() && rows[0]["other_admins"].as<int64_t>() == 0;
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

        std::vector<UserListEntry> users;
        users.reserve(rows.size());
        for (const auto& user : rows) {
            users.push_back({user["id"].as<int32_t>(), user["email"].as<std::string>(), user["first_name"].as<std::string>(),
                user["family_name"].as<std::string>(), user["organisation"].as<std::string>()});
        }

        auto json_result = sgrn::json::write(users);
        if (json_result.hasError()) {
            co_return createErrorResponse("Failed to serialize users response", k500InternalServerError, "AdminApi");
        }
        std::string response_json = std::move(json_result).value();
        co_return createJsonResponse(std::move(response_json), k200OK);
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
        payload_opt->domain = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(payload_opt->domain));
        if (!payload_opt->domain.empty()) {
            auto dom = co_await db->execSqlCoro(
                "SELECT 1 FROM core.domains WHERE organisation = $1 AND name = $2", payload_opt->organisation, payload_opt->domain);
            if (dom.empty()) {
                co_return createErrorResponse(
                    "Unknown domain '" + payload_opt->domain + "' in organisation '" + payload_opt->organisation + "'", k400BadRequest,
                    "AdminApi");
            }
        }
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

        ServiceRegistrationResponse resp{true, "Automated service registered successfully", res[0]["id"].as<int32_t>(),
            res[0]["name"].as<std::string>(), res[0]["token"].as<std::string>(), res[0]["token_secret"].as<std::string>(),
            res[0]["organisation"].as<std::string>(), kind};

        // Sync: insert the new public service token into local RAM sorted vector
        // and broadcast PG NOTIFY so all cluster nodes update their lookup vectors.
        const std::string new_serv_token = res[0]["token"].as<std::string>();
        sgrn::datastore::session::SessionStore::instance().registerServiceToken(new_serv_token);
        co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + new_serv_token);

        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();
        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "service.created", "automated_service",
            std::to_string(res[0]["id"].as<int32_t>()), tsp_req->getPeerAddr().toIp(), *json, "success");

        co_return createTypedJsonResponse(resp, k201Created);
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
    if (id_str.empty() || !json || !(json->isMember("metadata") || json->isMember("description"))) {
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
        // Description update rides the same PATCH: the service registry
        // previously had no way to change the separate description column
        // after registration. Empty string clears it.
        const bool has_description = json->isMember("description") && (*json)["description"].isString();
        const std::string description = has_description ? sgrn::utils::strings::trim((*json)["description"].asString()) : "";

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        Json::Value meta = json->isMember("metadata") ? (*json)["metadata"] : Json::Value(Json::objectValue);
        const std::string meta_str = Json::writeString(wb, meta);

        // Update with ORG check to ensure an admin from Org A cannot edit automated services from Org B.
        size_t affected = 0;
        if (has_description) {
            std::optional<std::string> desc_param = description.empty() ? std::nullopt : std::optional<std::string>(description);
            auto r = co_await db->execSqlCoro(
                "UPDATE core.automated_services SET metadata = metadata || $1::jsonb, description = $3 WHERE id = $2 AND organisation = $4",
                meta_str, automated_service_id, desc_param, org);
            affected = r.affectedRows();
        } else {
            auto r = co_await db->execSqlCoro(
                "UPDATE core.automated_services SET metadata = metadata || $1::jsonb WHERE id = $2 AND organisation = $3", meta_str,
                automated_service_id, org);
            affected = r.affectedRows();
        }

        if (affected == 0) {
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
        const Json::Value metadata = parseMetadataValue(updated["metadata"].as<std::string>());
        Json::StreamWriterBuilder metadata_writer;
        metadata_writer["indentation"] = "";
        const sgrn::json::RawJson metadata_raw(Json::writeString(metadata_writer, metadata));
        std::optional<sgrn::json::RawJson> kind_raw;
        if (metadata.isObject() && metadata.isMember("kind") && !metadata["kind"].isNull()) {
            kind_raw = sgrn::json::RawJson(Json::writeString(metadata_writer, metadata["kind"]));
        }
        ServiceRotationResponse resp{true, updated["id"].as<int32_t>(), updated["name"].as<std::string>(),
            updated["token"].as<std::string>(), updated["token_secret"].as<std::string>(), updated["status"].as<std::string>() == "active",
            updated["status"].as<std::string>(), updated["organisation"].as<std::string>(), updated["created_at"].as<std::string>(),
            metadata_raw, std::move(kind_raw)};

        // Sync: old public token is replaced — deregister old UUID from sorted vector
        // and register new UUID, broadcasting PG NOTIFY to all cluster nodes.
        if (!old_tok.empty()) {
            sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(old_tok);
            co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + old_tok);
        }
        const std::string new_tok = updated["token"].as<std::string>();
        sgrn::datastore::session::SessionStore::instance().registerServiceToken(new_tok);
        co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + new_tok);

        co_return createTypedJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to rotate credentials: {}", e.what());
        co_return createErrorResponse(std::string("Failed to rotate credentials: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleSetUserStatus(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t target_id = 0;
        try {
            target_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("status") || !(*json)["status"].isString()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }
        const std::string status = (*json)["status"].asString();
        if (status != "active" && status != "suspended") {
            co_return createErrorResponse("status must be 'active' or 'suspended'", k400BadRequest, "AdminApi");
        }
        if (target_id == ctx.actor_id) {
            co_return createErrorResponse("Forbidden: you cannot change your own status", k403Forbidden, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        auto target = co_await loadUserTarget(db, target_id, ctx.organisation, is_platform);
        if (!target.has_value()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        if (target->is_platform_admin && !is_platform) {
            co_return createErrorResponse("Forbidden: only a platform admin may change a platform admin", k403Forbidden, "AdminApi");
        }
        if (status == "suspended" && co_await isLastOrgAdmin(db, target_id)) {
            co_return createErrorResponse("Conflict: cannot suspend the last organisation admin", k409Conflict, "AdminApi");
        }

        // trg_revoke_user_sessions terminates live sessions when status leaves
        // 'active' or is_active drops (WP2 migration).
        co_await db->execSqlCoro(
            "UPDATE core.users SET status = $2::text, is_active = ($2::text = 'active') WHERE id = $1", target_id, status);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(target_id);

        Json::Value details(Json::objectValue);
        details["status"] = status;
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.status_changed",
            "user", std::to_string(target_id), tsp_req->getPeerAddr().toIp(), details, "success");

        co_return createTypedJsonResponse(UserStatusResponse{true, status}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to set user status: {}", e.what());
        co_return createErrorResponse(std::string("Failed to set user status: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteUser(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t target_id = 0;
        try {
            target_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        if (target_id == ctx.actor_id) {
            co_return createErrorResponse("Forbidden: you cannot delete your own account", k403Forbidden, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        auto target = co_await loadUserTarget(db, target_id, ctx.organisation, is_platform);
        if (!target.has_value()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        if (target->is_platform_admin && !is_platform) {
            co_return createErrorResponse("Forbidden: only a platform admin may delete a platform admin", k403Forbidden, "AdminApi");
        }
        if (target->status == "active" && co_await isLastOrgAdmin(db, target_id)) {
            co_return createErrorResponse("Conflict: cannot delete the last organisation admin", k409Conflict, "AdminApi");
        }

        // Soft delete: trg_revoke_user_sessions ends live sessions.
        co_await db->execSqlCoro("UPDATE core.users SET deleted_at = now(), is_active = false WHERE id = $1", target_id);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(target_id);

        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.deleted",
            "user", std::to_string(target_id), tsp_req->getPeerAddr().toIp(), Json::objectValue, "success");
        co_return createTypedJsonResponse(DeletedUserResponse{true, target_id}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to delete user: {}", e.what());
        co_return createErrorResponse(std::string("Failed to delete user: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleResetUserPassword(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t target_id = 0;
        try {
            target_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("new_password") || !(*json)["new_password"].isString()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }
        const std::string new_password = (*json)["new_password"].asString();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        auto target = co_await loadUserTarget(db, target_id, ctx.organisation, is_platform);
        if (!target.has_value()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        if (target->is_platform_admin && !is_platform) {
            co_return createErrorResponse(
                "Forbidden: only a platform admin may reset a platform admin's password", k403Forbidden, "AdminApi");
        }
        if (new_password.size() < 12) {
            co_return createErrorResponse("new_password must be at least 12 characters", k400BadRequest, "AdminApi");
        }
        if (sgrn::utils::strings::toLower(new_password) == sgrn::utils::strings::toLower(target->email)) {
            co_return createErrorResponse("new_password must differ from the user's email", k400BadRequest, "AdminApi");
        }

        // trg_hash_password only runs BEFORE INSERT — hash explicitly here.
        co_await db->execSqlCoro("UPDATE core.users SET password = crypt($2, gen_salt('bf')) WHERE id = $1", target_id, new_password);
        // Sessions do not survive a credential change; the enqueue trigger
        // broadcasts the revoke to every instance.
        co_await db->execSqlCoro("UPDATE core.sessions SET terminated_at = now(), termination_reason = 'password_changed' "
                                 "WHERE user_id = $1 AND terminated_at IS NULL",
            target_id);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(target_id);

        Json::Value details(Json::objectValue); // never the password itself
        details["sessions_revoked"] = true;
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "user.password_reset",
            "user", std::to_string(target_id), tsp_req->getPeerAddr().toIp(), details, "success");

        co_return createTypedJsonResponse(SuccessResponse{}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to reset user password: {}", e.what());
        co_return createErrorResponse(std::string("Failed to reset user password: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleSetAutomatedServiceStatus(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t service_id = 0;
        try {
            service_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("status") || !(*json)["status"].isString()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }
        const std::string status = (*json)["status"].asString();
        if (status != "active" && status != "degraded" && status != "offline" && status != "maintenance") {
            co_return createErrorResponse(
                "status must be one of 'active', 'degraded', 'offline' or 'maintenance'", k400BadRequest, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Org-scoped unless platform admin. Token registration (SessionStore)
        // keys on status = 'active' AND deleted_at IS NULL, so a non-active
        // service loses its credential lookup immediately — the same gate
        // loadActiveTokens applies at boot.
        std::string sql = "UPDATE core.automated_services SET status = $1::text "
                          "WHERE id = $2 AND deleted_at IS NULL RETURNING token::text";
        std::vector<std::string> binds = {status, std::to_string(service_id)};
        if (!is_platform) {
            sql += " AND organisation = $3";
            binds.push_back(ctx.organisation);
        }
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, binds);
        if (res.empty()) {
            // missing OR other tenant — same answer
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        const std::string token = res[0]["token"].isNull() ? "" : res[0]["token"].as<std::string>();
        if (!token.empty()) {
            if (status == "active") {
                sgrn::datastore::session::SessionStore::instance().registerServiceToken(token);
                co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "register:" + token);
            } else {
                sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(token);
                co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + token);
            }
        }

        Json::Value details(Json::objectValue);
        details["status"] = status;
        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name,
            "service.status_changed", "automated_service", std::to_string(service_id), tsp_req->getPeerAddr().toIp(), details, "success");

        co_return createTypedJsonResponse(ServiceStatusResponse{true, service_id, status}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to set automated service status: {}", e.what());
        co_return createErrorResponse(std::string("Failed to set automated service status: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteAutomatedService(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t service_id = 0;
        try {
            service_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        std::string sql =
            "UPDATE core.automated_services SET deleted_at = now() WHERE id = $1 AND deleted_at IS NULL RETURNING token::text";
        std::vector<std::string> binds = {std::to_string(service_id)};
        if (!is_platform) {
            binds.push_back(ctx.organisation);
            sql += " AND organisation = $2";
        }
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, binds);
        if (res.empty()) {
            // missing OR other tenant — same answer
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        const std::string token = res[0]["token"].isNull() ? "" : res[0]["token"].as<std::string>();
        if (!token.empty()) {
            sgrn::datastore::session::SessionStore::instance().deregisterServiceToken(token);
            co_await db->execSqlCoro("SELECT pg_notify('service_token_update', $1)", "deregister:" + token);
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "service.deleted",
            "automated_service", std::to_string(service_id), tsp_req->getPeerAddr().toIp(), Json::objectValue, "success");
        co_return createTypedJsonResponse(DeletedServiceResponse{true, service_id}, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to delete automated service: {}", e.what());
        co_return createErrorResponse(std::string("Failed to delete automated service: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
