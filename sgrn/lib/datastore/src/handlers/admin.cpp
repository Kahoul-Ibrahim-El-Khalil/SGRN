#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>
#include <sgrn/debug.hpp>
#include <json/json.h>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef DEBUG_ADMIN_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

#define INFO_LOG(msg, ...) SGRN_INFO("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/handlers/admin.hpp>
#include <sgrn/datastore/services/WebhookService.hpp>
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
        ERROR_LOG("User registration error: {}", e.what());
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
        ERROR_LOG("Automated service registration DB error: {}", e.base().what());
        co_return createErrorResponse(std::string("Automated service registration failed: ") + e.base().what(), k400BadRequest, "AdminApi");
    } catch (const std::exception& e) {
        ERROR_LOG("Automated service registration error: {}", e.what());
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
        ERROR_LOG("Failed to update automated service metadata: {}", e.what());
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
        ERROR_LOG("Failed to rotate credentials: {}", e.what());
        co_return createErrorResponse(std::string("Failed to rotate credentials: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleGetEndpoints(HttpRequestPtr tsp_req) {
    Json::Value endpoints = Json::arrayValue;
    // Basic discovery - could be expanded to list all registered routes
    endpoints.append("/api/v1/admin/status");
    endpoints.append("/api/v1/admin/users");
    endpoints.append("/api/v1/admin/metaprobe/sessions");
    endpoints.append("/api/v1/admin/storage/overview");
    endpoints.append("/api/v1/admin/storage/orphans");
    endpoints.append("/api/v1/admin/storage/orphans/purge");
    endpoints.append("/api/v1/admin/storage/search");
    endpoints.append("/api/v1/admin/system/config");
    endpoints.append("/api/v1/admin/storage/formats");
    endpoints.append("/api/v1/admin/quotas");
    endpoints.append("/api/v1/admin/permissions");
    endpoints.append("/api/v1/admin/analytics/overview");
    endpoints.append("/api/v1/admin/analytics/breakdown");
    co_return createJsonResponse(endpoints, k200OK);
}

namespace
{

// Joined permission row shared by list/grant responses.
Json::Value permissionRowJson(int64_t t_id, int32_t t_user_id, const std::string& t_email, const std::string& t_organisation,
    const std::string& t_domain, const std::string& t_subpath, bool t_read, bool t_write, bool t_delete) {
    Json::Value e;
    e["id"] = Json::Int64(t_id);
    e["user_id"] = t_user_id;
    e["email"] = t_email;
    e["organisation"] = t_organisation;
    e["domain"] = t_domain;
    e["allowed_subpath"] = t_subpath;
    e["can_read"] = t_read;
    e["can_write"] = t_write;
    e["can_delete"] = t_delete;
    return e;
}

bool rowBool(const drogon::orm::Row& t_row, const char* t_col) {
    if (t_row[t_col].isNull()) {
        return false;
    }
    try {
        return t_row[t_col].as<bool>();
    } catch (const std::exception&) {
        return t_row[t_col].as<std::string>() == "t";
    }
}

} // namespace

Task<HttpResponsePtr> AdminApiHandler::handleListPermissions(HttpRequestPtr tsp_req) {
    try {
        const std::string user_id_raw = tsp_req->getParameter("user_id");
        const std::string email = tsp_req->getParameter("email");
        const std::string domain = tsp_req->getParameter("domain");
        const std::string organisation = tsp_req->getParameter("organisation");
        std::size_t limit = 100;
        try {
            const std::size_t asked = static_cast<std::size_t>(std::stoul(tsp_req->getParameter("limit")));
            if (asked > 0) {
                limit = std::min(asked, static_cast<std::size_t>(1000));
            }
        } catch (const std::exception&) {
        }
        int32_t user_id = 0;
        if (!user_id_raw.empty()) {
            try {
                user_id = static_cast<int32_t>(std::stoi(user_id_raw));
            } catch (const std::exception&) {
                co_return createErrorResponse("user_id must be an integer", k400BadRequest, "AdminApi");
            }
            if (user_id <= 0) {
                co_return createErrorResponse("user_id must be positive", k400BadRequest, "AdminApi");
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Filters are ANDed; each absent filter matches everything. LIKE
        // patterns go through parameters (no string splicing of values).
        auto rows = co_await db->execSqlCoro(
            "SELECT p.id, p.user_id, u.email, p.organisation, p.domain, p.allowed_subpath, p.can_read, p.can_write, p.can_delete "
            "FROM core.user_domain_permissions p JOIN core.users u ON u.id = p.user_id "
            "WHERE u.deleted_at IS NULL "
            "AND ($1 = 0 OR p.user_id = $1) "
            "AND ($2 = '' OR u.email ILIKE '%' || $2 || '%') "
            "AND ($3 = '' OR p.domain = $3) "
            "AND ($4 = '' OR p.organisation = $4) "
            "ORDER BY u.email, p.domain LIMIT $5",
            user_id, email, domain, organisation, static_cast<int64_t>(limit));

        Json::Value list = Json::arrayValue;
        for (const auto& row : rows) {
            list.append(permissionRowJson(row["id"].as<int64_t>(), row["user_id"].as<int32_t>(), row["email"].as<std::string>(),
                row["organisation"].as<std::string>(), row["domain"].as<std::string>(), row["allowed_subpath"].as<std::string>(),
                rowBool(row, "can_read"), rowBool(row, "can_write"), rowBool(row, "can_delete")));
        }
        Json::Value out;
        out["permissions"] = std::move(list);
        out["count"] = static_cast<Json::Int64>(rows.size());
        co_return createJsonResponse(out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Permission list DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ERROR_LOG("Permission list error: {}", e.what());
        co_return createErrorResponse(std::string("Failed to list permissions: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleGrantPermission(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    if (json == nullptr || !json->isObject()) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }
    try {
        // Resolve the user by id or email (must exist and not be soft-deleted).
        int32_t user_id = 0;
        std::string email;
        std::string user_org;
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        if (json->isMember("user_id") && (*json)["user_id"].isInt() && (*json)["user_id"].asInt() > 0) {
            user_id = (*json)["user_id"].asInt();
            auto hit = co_await db->execSqlCoro("SELECT email, organisation FROM core.users WHERE id = $1 AND deleted_at IS NULL", user_id);
            if (hit.empty()) {
                co_return createErrorResponse("No such user (or soft-deleted)", k404NotFound, "AdminApi");
            }
            email = hit[0]["email"].as<std::string>();
            user_org = hit[0]["organisation"].as<std::string>();
        } else if (json->isMember("email") && (*json)["email"].isString() && !(*json)["email"].asString().empty()) {
            email = sgrn::utils::strings::trim((*json)["email"].asString());
            auto hit = co_await db->execSqlCoro("SELECT id, organisation FROM core.users WHERE email = $1 AND deleted_at IS NULL", email);
            if (hit.empty()) {
                co_return createErrorResponse("No such user (or soft-deleted)", k404NotFound, "AdminApi");
            }
            user_id = hit[0]["id"].as<int32_t>();
            user_org = hit[0]["organisation"].as<std::string>();
        } else {
            co_return createErrorResponse("user_id or email is required", k400BadRequest, "AdminApi");
        }

        if (!json->isMember("domain") || !(*json)["domain"].isString() || (*json)["domain"].asString().empty()) {
            co_return createErrorResponse("domain is required", k400BadRequest, "AdminApi");
        }
        const std::string domain = sgrn::utils::strings::trim((*json)["domain"].asString());

        // Organisation defaults to the user's own; cross-org grants are
        // refused (a user belongs to exactly one organisation, and the
        // enforcement lookup is domain-scoped within it).
        std::string organisation = user_org;
        if (json->isMember("organisation") && (*json)["organisation"].isString() && !(*json)["organisation"].asString().empty()) {
            organisation = sgrn::utils::strings::trim((*json)["organisation"].asString());
            if (organisation != user_org) {
                co_return createErrorResponse("organisation must match the user's own organisation", k400BadRequest, "AdminApi");
            }
        }

        // The FK would 500 on an unknown domain — pre-check for a clean 400.
        auto dom = co_await db->execSqlCoro("SELECT 1 FROM core.domains WHERE organisation = $1 AND name = $2", organisation, domain);
        if (dom.empty()) {
            co_return createErrorResponse(
                "Unknown domain '" + domain + "' in organisation '" + organisation + "'", k400BadRequest, "AdminApi");
        }

        std::string subpath = "/";
        if (json->isMember("allowed_subpath") && (*json)["allowed_subpath"].isString()) {
            subpath = sgrn::utils::strings::trim((*json)["allowed_subpath"].asString());
            if (subpath.empty()) {
                subpath = "/";
            }
        }
        if (subpath.front() != '/') {
            co_return createErrorResponse("allowed_subpath must start with '/'", k400BadRequest, "AdminApi");
        }
        auto flag = [&](const char* t_key, bool t_dflt) {
            return (json->isMember(t_key) && (*json)[t_key].isBool()) ? (*json)[t_key].asBool() : t_dflt;
        };
        const bool can_read = flag("can_read", true);
        const bool can_write = flag("can_write", true);
        const bool can_delete = flag("can_delete", false);

        auto before = co_await db->execSqlCoro("SELECT COUNT(*) FROM core.user_domain_permissions WHERE user_id = $1", user_id);
        const bool first_grant = !before.empty() && before[0][0].as<std::string>() == "0";

        auto upserted = co_await db->execSqlCoro(
            "INSERT INTO core.user_domain_permissions (user_id, organisation, domain, allowed_subpath, can_read, can_write, can_delete) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7) "
            "ON CONFLICT (user_id, organisation, domain) DO UPDATE SET allowed_subpath = EXCLUDED.allowed_subpath, "
            "can_read = EXCLUDED.can_read, can_write = EXCLUDED.can_write, can_delete = EXCLUDED.can_delete, "
            "updated_at = now() "
            "RETURNING id, allowed_subpath, can_read, can_write, can_delete",
            user_id, organisation, domain, subpath, can_read, can_write, can_delete);
        const auto& row = upserted[0];
        Json::Value out = permissionRowJson(row["id"].as<int64_t>(), user_id, email, organisation, domain,
            row["allowed_subpath"].as<std::string>(), rowBool(row, "can_read"), rowBool(row, "can_write"), rowBool(row, "can_delete"));
        if (first_grant) {
            out["note"] = "First domain grant for this user — storage access unlocked (zero-trust default was deny-all).";
        }
        co_return createJsonResponse(out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Permission grant DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ERROR_LOG("Permission grant error: {}", e.what());
        co_return createErrorResponse(std::string("Failed to grant permission: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRevokePermission(HttpRequestPtr tsp_req) {
    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        std::string revoke_sql;
        std::vector<std::string> revoke_binds;
        std::string id_desc;
        const std::string id_raw = tsp_req->getParameter("id");
        if (!id_raw.empty()) {
            int64_t id = 0;
            try {
                id = std::stoll(id_raw);
            } catch (const std::exception&) {
                co_return createErrorResponse("id must be an integer", k400BadRequest, "AdminApi");
            }
            revoke_sql = "DELETE FROM core.user_domain_permissions WHERE id = $1 RETURNING user_id";
            revoke_binds.push_back(std::to_string(id));
            id_desc = "id " + std::to_string(id);
        } else {
            const std::string user_id_raw = tsp_req->getParameter("user_id");
            const std::string organisation = tsp_req->getParameter("organisation");
            const std::string domain = tsp_req->getParameter("domain");
            int32_t user_id = 0;
            try {
                user_id = static_cast<int32_t>(std::stoi(user_id_raw));
            } catch (const std::exception&) {
                co_return createErrorResponse("supply ?id= or ?user_id=&organisation=&domain=", k400BadRequest, "AdminApi");
            }
            if (user_id <= 0 || organisation.empty() || domain.empty()) {
                co_return createErrorResponse("supply ?id= or ?user_id=&organisation=&domain=", k400BadRequest, "AdminApi");
            }
            revoke_sql = "DELETE FROM core.user_domain_permissions WHERE user_id = $1 AND organisation = $2 AND domain = $3 "
                         "RETURNING user_id";
            revoke_binds = {std::to_string(user_id), organisation, domain};
            id_desc = "user " + std::to_string(user_id) + " / " + domain;
        }
        // Route through execSqlCoroVec: its const-ref vector parameter selects
        // Drogon's vector-bind overload. Calling execSqlCoro directly with a
        // mutable vector lvalue resolves to the variadic template instead and
        // fails to compile (SqlBinder has no vector operator<<).
        auto deleted = co_await sgrn::datastore::core::execSqlCoroVec(db, revoke_sql, revoke_binds);
        if (deleted.empty()) {
            co_return createErrorResponse("No such permission grant", k404NotFound, "AdminApi");
        }
        const int32_t affected_user = deleted[0]["user_id"].as<int32_t>();
        auto remaining = co_await db->execSqlCoro("SELECT COUNT(*) FROM core.user_domain_permissions WHERE user_id = $1", affected_user);

        Json::Value out;
        out["success"] = true;
        out["revoked"] = id_desc;
        if (!remaining.empty() && remaining[0][0].as<std::string>() == "0") {
            out["warning"] = "User now holds no domain grants — zero-trust default-deny locks them out of storage entirely.";
        }
        co_return createJsonResponse(out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Permission revoke DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ERROR_LOG("Permission revoke error: {}", e.what());
        co_return createErrorResponse(std::string("Failed to revoke permission: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

// ── RBAC Roles & Permissions ──────────────────────────────────────────────────
Task<HttpResponsePtr> AdminApiHandler::handleGetRoles(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto res = co_await db->execSqlCoro("SELECT id, name, description, permissions, is_system, created_at, updated_at "
                                            "FROM core.roles WHERE organisation = $1 ORDER BY name",
            org);

        Json::Value roles_arr = Json::arrayValue;
        for (const auto& row : res) {
            Json::Value r;
            r["id"] = row["id"].as<int32_t>();
            r["name"] = row["name"].as<std::string>();
            r["description"] = row["description"].isNull() ? "" : row["description"].as<std::string>();
            r["is_system"] = row["is_system"].as<bool>();
            r["created_at"] = row["created_at"].as<std::string>();
            r["updated_at"] = row["updated_at"].as<std::string>();

            const std::string raw_perms = row["permissions"].as<std::string>();
            Json::CharReaderBuilder builder;
            Json::Value perms_json;
            std::string errs;
            const std::unique_ptr<Json::CharReader> up_reader(builder.newCharReader());
            if (up_reader->parse(raw_perms.data(), raw_perms.data() + raw_perms.size(), &perms_json, &errs)) {
                r["permissions"] = perms_json;
            } else {
                r["permissions"] = Json::arrayValue;
            }
            roles_arr.append(r);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["roles"] = roles_arr;
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to get roles: {}", e.what());
        co_return createErrorResponse(std::string("Failed to get roles: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleCreateRole(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("name") || !(*json)["name"].isString()) {
            co_return createErrorResponse("Role 'name' string is required", k400BadRequest, "AdminApi");
        }

        const std::string name = (*json)["name"].asString();
        const std::string desc =
            json->isMember("description") && (*json)["description"].isString() ? (*json)["description"].asString() : "";
        Json::Value perms = json->isMember("permissions") && (*json)["permissions"].isArray() ? (*json)["permissions"] : Json::arrayValue;

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string perms_str = Json::writeString(wb, perms);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto res = co_await db->execSqlCoro("INSERT INTO core.roles (organisation, name, description, permissions, is_system) "
                                            "VALUES ($1, $2, $3, $4::jsonb, false) RETURNING id",
            org, name, desc, perms_str);

        if (res.empty()) {
            co_return createErrorResponse(AdminApiError::DbError);
        }

        const int32_t role_id = res[0]["id"].as<int32_t>();

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.created", "role",
            std::to_string(role_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["role_id"] = role_id;
        resp["message"] = "Custom role created successfully";
        co_return createJsonResponse(resp, k201Created);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to create role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to create role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateRole(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const int32_t role_id = std::stoi(t_id);
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";

        std::string update_sql = "UPDATE core.roles SET updated_at = NOW()";
        std::vector<std::string> params = {org, std::to_string(role_id)};
        int param_idx = 3;

        if (json->isMember("description") && (*json)["description"].isString()) {
            update_sql += ", description = $" + std::to_string(param_idx++);
            params.push_back((*json)["description"].asString());
        }
        if (json->isMember("permissions") && (*json)["permissions"].isArray()) {
            update_sql += ", permissions = $" + std::to_string(param_idx++) + "::jsonb";
            params.push_back(Json::writeString(wb, (*json)["permissions"]));
        }

        update_sql += " WHERE organisation = $1 AND id = $2 AND is_system = false";

        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, update_sql, params);
        if (res.affectedRows() == 0) {
            co_return createErrorResponse("Role not found or is a protected system role", k400BadRequest, "AdminApi");
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.updated", "role",
            std::to_string(role_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role updated successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to update role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteRole(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const int32_t role_id = std::stoi(t_id);
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto res =
            co_await db->execSqlCoro("DELETE FROM core.roles WHERE organisation = $1 AND id = $2 AND is_system = false", org, role_id);

        if (res.affectedRows() == 0) {
            co_return createErrorResponse("Role not found or is a protected system role", k400BadRequest, "AdminApi");
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.deleted", "role",
            std::to_string(role_id), tsp_req->getPeerAddr().toIp(), Json::objectValue, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role deleted successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to delete role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to delete role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleAssignRole(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("role_id")) {
            co_return createErrorResponse("'role_id' is required", k400BadRequest, "AdminApi");
        }

        const int32_t role_id = (*json)["role_id"].asInt();
        std::optional<int32_t> user_id;
        std::optional<int32_t> service_id;

        if (json->isMember("user_id") && (*json)["user_id"].isInt()) {
            user_id = (*json)["user_id"].asInt();
        } else if (json->isMember("automated_service_id") && (*json)["automated_service_id"].isInt()) {
            service_id = (*json)["automated_service_id"].asInt();
        } else {
            co_return createErrorResponse("Specify either 'user_id' or 'automated_service_id'", k400BadRequest, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        co_await db->execSqlCoro(
            "INSERT INTO core.user_roles (user_id, automated_service_id, role_id) VALUES ($1, $2, $3)", user_id, service_id, role_id);

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.assigned",
            user_id.has_value() ? "user" : "automated_service",
            user_id.has_value() ? std::to_string(*user_id) : std::to_string(*service_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role assigned successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to assign role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to assign role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRevokeRole(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("role_id")) {
            co_return createErrorResponse("'role_id' is required", k400BadRequest, "AdminApi");
        }

        const int32_t role_id = (*json)["role_id"].asInt();
        std::optional<int32_t> user_id;
        std::optional<int32_t> service_id;

        if (json->isMember("user_id") && (*json)["user_id"].isInt()) {
            user_id = (*json)["user_id"].asInt();
        } else if (json->isMember("automated_service_id") && (*json)["automated_service_id"].isInt()) {
            service_id = (*json)["automated_service_id"].asInt();
        } else {
            co_return createErrorResponse("Specify either 'user_id' or 'automated_service_id'", k400BadRequest, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        if (user_id.has_value()) {
            co_await db->execSqlCoro("DELETE FROM core.user_roles WHERE user_id = $1 AND role_id = $2", *user_id, role_id);
        } else {
            co_await db->execSqlCoro("DELETE FROM core.user_roles WHERE automated_service_id = $1 AND role_id = $2", *service_id, role_id);
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.revoked",
            user_id.has_value() ? "user" : "automated_service",
            user_id.has_value() ? std::to_string(*user_id) : std::to_string(*service_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role revoked successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to revoke role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to revoke role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

// ── Audit Trail System ────────────────────────────────────────────────────────
Task<HttpResponsePtr> AdminApiHandler::handleGetAuditLogs(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();

        const std::string actor_type = tsp_req->getParameter("actor_type");
        const std::string action = tsp_req->getParameter("action");
        const std::string status = tsp_req->getParameter("status");
        const std::string limit_raw = tsp_req->getParameter("limit");
        const std::string offset_raw = tsp_req->getParameter("offset");

        int32_t limit = limit_raw.empty() ? 50 : std::stoi(limit_raw);
        int32_t offset = offset_raw.empty() ? 0 : std::stoi(offset_raw);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        std::string query = "SELECT id, organisation, actor_type, actor_id, actor_name, action, target_type, target_id, ip::text, details, "
                            "status, created_at "
                            "FROM core.audit_logs WHERE organisation = $1";
        std::vector<std::string> params = {org};
        int p_idx = 2;

        if (!actor_type.empty()) {
            query += " AND actor_type = $" + std::to_string(p_idx++);
            params.push_back(actor_type);
        }
        if (!action.empty()) {
            query += " AND action = $" + std::to_string(p_idx++);
            params.push_back(action);
        }
        if (!status.empty()) {
            query += " AND status = $" + std::to_string(p_idx++);
            params.push_back(status);
        }

        const int limit_idx = p_idx++;
        const int offset_idx = p_idx++;
        query += " ORDER BY created_at DESC LIMIT $" + std::to_string(limit_idx) + " OFFSET $" + std::to_string(offset_idx);
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(offset));

        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, query, params);

        Json::Value logs_arr = Json::arrayValue;
        for (const auto& row : res) {
            Json::Value entry;
            entry["id"] = row["id"].as<int64_t>();
            entry["organisation"] = row["organisation"].as<std::string>();
            entry["actor_type"] = row["actor_type"].as<std::string>();
            entry["actor_id"] = row["actor_id"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["actor_id"].as<int32_t>());
            entry["actor_name"] = row["actor_name"].as<std::string>();
            entry["action"] = row["action"].as<std::string>();
            entry["target_type"] = row["target_type"].isNull() ? "" : row["target_type"].as<std::string>();
            entry["target_id"] = row["target_id"].isNull() ? "" : row["target_id"].as<std::string>();
            entry["ip"] = row["ip"].as<std::string>();
            entry["status"] = row["status"].as<std::string>();
            entry["created_at"] = row["created_at"].as<std::string>();

            const std::string raw_det = row["details"].isNull() ? "{}" : row["details"].as<std::string>();
            Json::CharReaderBuilder builder;
            Json::Value det_json;
            std::string errs;
            const std::unique_ptr<Json::CharReader> up_reader(builder.newCharReader());
            if (up_reader->parse(raw_det.data(), raw_det.data() + raw_det.size(), &det_json, &errs)) {
                entry["details"] = det_json;
            } else {
                entry["details"] = Json::objectValue;
            }
            logs_arr.append(entry);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["audit_logs"] = logs_arr;
        resp["limit"] = limit;
        resp["offset"] = offset;
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to fetch audit logs: {}", e.what());
        co_return createErrorResponse(std::string("Failed to fetch audit logs: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handlePurgeAuditLogs(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        const std::string days_raw = tsp_req->getParameter("older_than_days");
        int32_t days = days_raw.empty() ? 30 : std::stoi(days_raw);
        if (days < 1)
            days = 1;

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto res = co_await db->execSqlCoro(
            "DELETE FROM core.audit_logs WHERE organisation = $1 AND created_at < NOW() - ($2 || ' days')::interval RETURNING id", org,
            std::to_string(days));

        const int64_t purged_count = res.size();

        // Auditable Clearance: Log an immutable audit entry documenting the purge operation!
        Json::Value details;
        details["older_than_days"] = days;
        details["records_purged"] = purged_count;
        details["cleared_by"] = actor_name;

        co_await sgrn::datastore::audit::AuditLogger::log(
            db, org, "user", actor_id, actor_name, "audit.purged", "audit_logs", org, tsp_req->getPeerAddr().toIp(), details, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["records_purged"] = purged_count;
        resp["older_than_days"] = days;
        resp["message"] = "Audit logs purged successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to purge audit logs: {}", e.what());
        co_return createErrorResponse(std::string("Failed to purge audit logs: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

// ── Quotas & Rate Limits ──────────────────────────────────────────────────────
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
        ERROR_LOG("Failed to get quotas: {}", e.what());
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
        ERROR_LOG("Failed to update org quota: {}", e.what());
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
        ERROR_LOG("Failed to update user quota: {}", e.what());
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
        ERROR_LOG("Failed to update service quota: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update service quota: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleGetMetaProbeSessions(HttpRequestPtr tsp_req) {
    try {
        Json::Value sessions = co_await sgrn::datastore::session::SessionStore::instance().getActiveSessions();
        co_return createJsonResponse(sessions, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed to get live sessions: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleListWebhooks(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();

        auto endpoints = co_await sgrn::datastore::services::WebhookService::instance().getWebhooks(org);
        Json::Value list = Json::arrayValue;
        for (const auto& ep : endpoints) {
            Json::Value item;
            item["id"] = ep.id;
            item["organisation"] = ep.organisation;
            item["url"] = ep.url;
            item["is_active"] = ep.is_active;
            item["created_at"] = ep.created_at;
            list.append(item);
        }
        co_return createJsonResponse(list, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed listing webhooks: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRegisterWebhook(HttpRequestPtr tsp_req) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        auto json = tsp_req->getJsonObject();

        if (!json || !json->isMember("url") || !(*json)["url"].isString()) {
            co_return createErrorResponse("url is required", k400BadRequest, "AdminApi");
        }

        std::string url = (*json)["url"].asString();
        std::string secret = json->get("secret", "").asString();
        std::vector<std::string> events = {"user.signin", "user.signout", "service.signin"};

        auto ep_opt = co_await sgrn::datastore::services::WebhookService::instance().createWebhook(org, url, secret, events);
        if (!ep_opt) {
            co_return createErrorResponse("Failed creating webhook subscription", k400BadRequest, "AdminApi");
        }

        Json::Value resp;
        resp["id"] = ep_opt->id;
        resp["url"] = ep_opt->url;
        resp["organisation"] = ep_opt->organisation;
        resp["created_at"] = ep_opt->created_at;
        co_return createJsonResponse(resp, k201Created);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed registering webhook: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteWebhook(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const std::string org = session["user"]["organisation"].asString();
        int32_t webhook_id = std::stoi(t_id);

        bool deleted = co_await sgrn::datastore::services::WebhookService::instance().deleteWebhook(org, webhook_id);
        if (!deleted) {
            co_return createErrorResponse("Webhook not found", k404NotFound, "AdminApi");
        }

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Webhook deleted successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ERROR_LOG("Failed deleting webhook: {}", e.what());
        co_return createErrorResponse(AdminApiError::DbError);
    }
}

} // namespace sgrn::datastore::handlers::admin

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
