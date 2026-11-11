// admin-permissions.cpp — User permission administration.
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace
{
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
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        const std::string user_id_raw = tsp_req->getParameter("user_id");
        const std::string email = tsp_req->getParameter("email");
        const std::string domain = tsp_req->getParameter("domain");
        std::string organisation = tsp_req->getParameter("organisation");

        if (!is_platform) {
            if (!organisation.empty() && organisation != session_org) {
                co_return createErrorResponse("Forbidden: cannot view permissions of another organisation", k403Forbidden, "AdminApi");
            }
            organisation = session_org;
        }

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
        ADMIN_ERROR_LOG("Permission list DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Permission list error: {}", e.what());
        co_return createErrorResponse(std::string("Failed to list permissions: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleGrantPermission(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    if (json == nullptr || !json->isObject()) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        // Resolve caller session user ID for self-grant prevention
        int32_t actor_id = 0;
        std::string actor_name;
        if (tsp_req->attributes()->find("session_json")) {
            const auto& session = tsp_req->attributes()->get<Json::Value>("session_json");
            if (session.isMember("user")) {
                if (session["user"].isMember("id") && session["user"]["id"].isInt()) {
                    actor_id = session["user"]["id"].asInt();
                }
                if (session["user"].isMember("email") && session["user"]["email"].isString()) {
                    actor_name = session["user"]["email"].asString();
                }
            }
        }

        // Escalation guard: self-grant is strictly forbidden (checked before DB dependency)
        if ((actor_id > 0 && json->isMember("user_id") && (*json)["user_id"].isInt() && (*json)["user_id"].asInt() == actor_id) ||
            (!actor_name.empty() && json->isMember("email") && (*json)["email"].isString() &&
                sgrn::utils::strings::trim((*json)["email"].asString()) == actor_name)) {
            auto db_res = sgrn::datastore::core::getDbClient();
            if (!db_res.hasError()) {
                co_await sgrn::datastore::audit::AuditLogger::log(db_res.value(), session_org, "user", actor_id, actor_name,
                    "permission.grant_rejected_self_grant", "user", std::to_string(actor_id), tsp_req->getPeerAddr().toIp(), *json,
                    "failed");
            }
            co_return createErrorResponse(
                "Forbidden: cannot grant permissions to yourself (self-grant prohibited)", k403Forbidden, "AdminApi");
        }

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

        if (!is_platform && user_org != session_org) {
            co_return createErrorResponse(
                "Forbidden: cannot grant permissions to users in another organisation", k403Forbidden, "AdminApi");
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
        if (!is_platform && organisation != session_org) {
            co_return createErrorResponse("Forbidden: cannot grant permissions for another organisation", k403Forbidden, "AdminApi");
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
        co_await sgrn::datastore::audit::AuditLogger::log(db, organisation, "user", actor_id, actor_name, "permission.granted", "user",
            std::to_string(user_id), tsp_req->getPeerAddr().toIp(), *json, "success");
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(user_id);
        co_return createJsonResponse(out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Permission grant DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Permission grant error: {}", e.what());
        co_return createErrorResponse(std::string("Failed to grant permission: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRevokePermission(HttpRequestPtr tsp_req) {
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

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
            if (!is_platform) {
                auto hit = co_await db->execSqlCoro("SELECT organisation FROM core.user_domain_permissions WHERE id = $1", id);
                if (hit.empty() || hit[0]["organisation"].as<std::string>() != session_org) {
                    co_return createErrorResponse("No such permission grant", k404NotFound, "AdminApi");
                }
            }
            revoke_sql = "DELETE FROM core.user_domain_permissions WHERE id = $1 RETURNING user_id";
            revoke_binds.push_back(std::to_string(id));
            id_desc = "id " + std::to_string(id);
        } else {
            const std::string user_id_raw = tsp_req->getParameter("user_id");
            std::string organisation = tsp_req->getParameter("organisation");
            const std::string domain = tsp_req->getParameter("domain");
            int32_t user_id = 0;
            try {
                user_id = static_cast<int32_t>(std::stoi(user_id_raw));
            } catch (const std::exception&) {
                co_return createErrorResponse("supply ?id= or ?user_id=&organisation=&domain=", k400BadRequest, "AdminApi");
            }
            if (user_id <= 0 || domain.empty()) {
                co_return createErrorResponse("supply ?id= or ?user_id=&organisation=&domain=", k400BadRequest, "AdminApi");
            }
            if (!is_platform) {
                if (!organisation.empty() && organisation != session_org) {
                    co_return createErrorResponse(
                        "Forbidden: cannot revoke permissions for another organisation", k403Forbidden, "AdminApi");
                }
                organisation = session_org;
            } else if (organisation.empty()) {
                organisation = session_org;
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
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(affected_user);
        co_return createJsonResponse(out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Permission revoke DB error: {}", e.base().what());
        co_return createErrorResponse(AdminApiError::DbError);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Permission revoke error: {}", e.what());
        co_return createErrorResponse(std::string("Failed to revoke permission: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
