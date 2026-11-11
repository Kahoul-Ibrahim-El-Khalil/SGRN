// admin-roles.cpp — Role administration.
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

Task<HttpResponsePtr> AdminApiHandler::handleGetRoles(HttpRequestPtr tsp_req) {
    try {
        const auto context = detail::requestContext(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        sgrn::datastore::services::admin::AdminRepository repository(db);
        auto res = co_await repository.listRoles(context.organisation);
        if (res.hasError())
            co_return createErrorResponse(AdminApiError::DbError);

        Json::Value roles_arr = Json::arrayValue;
        for (const auto& row : res.value()) {
            Json::Value r;
            r["id"] = row.id;
            r["name"] = row.name;
            r["description"] = row.description;
            r["is_system"] = row.system;
            r["created_at"] = row.created_at;
            r["updated_at"] = row.updated_at;

            const std::string& raw_perms = row.permissions;
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
        ADMIN_ERROR_LOG("Failed to get roles: {}", e.what());
        co_return createErrorResponse(std::string("Failed to get roles: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleCreateRole(HttpRequestPtr tsp_req) {
    try {
        const auto context = detail::requestContext(tsp_req);

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

        sgrn::datastore::services::admin::AdminRepository repository(db);
        auto res = co_await repository.createRole(context.organisation, name, desc, perms_str);
        if (res.hasError()) {
            co_return createErrorResponse(AdminApiError::DbError);
        }

        const int32_t role_id = res.value();

        co_await sgrn::datastore::audit::AuditLogger::log(db, context.organisation, "user", context.actor_id, context.actor_name,
            "role.created", "role", std::to_string(role_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["role_id"] = role_id;
        resp["message"] = "Custom role created successfully";
        co_return createJsonResponse(resp, k201Created);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to create role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to create role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateRole(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const int32_t role_id = std::stoi(t_id);
        const auto context = detail::requestContext(tsp_req);

        auto json = tsp_req->getJsonObject();
        if (!json) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        std::optional<std::string> description;
        std::optional<std::string> permissions;
        if (json->isMember("description") && (*json)["description"].isString())
            description = (*json)["description"].asString();
        if (json->isMember("permissions") && (*json)["permissions"].isArray()) {
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            permissions = Json::writeString(wb, (*json)["permissions"]);
        }

        sgrn::datastore::services::admin::AdminRepository repository(db);
        auto res = co_await repository.updateRole(role_id, context.organisation, description, permissions);
        if (res.hasError() || !res.value()) {
            co_return createErrorResponse("Role not found or is a protected system role", k400BadRequest, "AdminApi");
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, context.organisation, "user", context.actor_id, context.actor_name,
            "role.updated", "role", std::to_string(role_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role updated successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to update role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to update role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleDeleteRole(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const int32_t role_id = std::stoi(t_id);
        const auto context = detail::requestContext(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        sgrn::datastore::services::admin::AdminRepository repository(db);
        auto res = co_await repository.deleteRole(role_id, context.organisation);
        if (res.hasError() || !res.value()) {
            co_return createErrorResponse("Role not found or is a protected system role", k400BadRequest, "AdminApi");
        }

        co_await sgrn::datastore::audit::AuditLogger::log(db, context.organisation, "user", context.actor_id, context.actor_name,
            "role.deleted", "role", std::to_string(role_id), tsp_req->getPeerAddr().toIp(), Json::objectValue, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role deleted successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to delete role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to delete role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleAssignRole(HttpRequestPtr tsp_req) {
    try {
        const auto context = detail::requestContext(tsp_req);
        const std::string& org = context.organisation;
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);
        const int32_t actor_id = context.actor_id;
        const std::string& actor_name = context.actor_name;

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

        // Escalation guard: self-grant is strictly forbidden (checked before DB dependency)
        if (user_id.has_value() && *user_id == actor_id) {
            auto db_res = sgrn::datastore::core::getDbClient();
            if (!db_res.hasError()) {
                co_await sgrn::datastore::audit::AuditLogger::log(db_res.value(), org, "user", actor_id, actor_name,
                    "role.assign_rejected_self_grant", "user", std::to_string(*user_id), tsp_req->getPeerAddr().toIp(), *json, "failed");
            }
            co_return createErrorResponse("Forbidden: cannot assign roles to yourself (self-grant prohibited)", k403Forbidden, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();
        sgrn::datastore::services::admin::AdminRepository repository(db);
        const sgrn::datastore::services::admin::RoleTarget target{user_id, service_id};

        if (!is_platform) {
            auto role_hit = co_await repository.roleAccess(role_id);
            if (role_hit.hasError() || role_hit.value().organisation != org) {
                co_return createErrorResponse("Role not found in caller's organisation", k404NotFound, "AdminApi");
            }
            if (role_hit.value().system) {
                co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.assign_rejected_above_level",
                    "role", std::to_string(role_id), tsp_req->getPeerAddr().toIp(), *json, "failed");
                co_return createErrorResponse("Forbidden: cannot assign system roles (above level)", k403Forbidden, "AdminApi");
            }
            if (user_id.has_value()) {
                auto u_hit = co_await repository.userOrganisation(*user_id);
                if (u_hit.hasError() || u_hit.value().organisation != org) {
                    co_return createErrorResponse("User not found in caller's organisation", k404NotFound, "AdminApi");
                }
            }
            if (service_id.has_value()) {
                auto s_hit = co_await repository.serviceOrganisation(*service_id);
                if (s_hit.hasError() || s_hit.value().organisation != org) {
                    co_return createErrorResponse("Automated service not found in caller's organisation", k404NotFound, "AdminApi");
                }
            }
        }

        auto assigned = co_await repository.assignRole(target, role_id);
        if (assigned.hasError())
            co_return createErrorResponse(AdminApiError::DbError);

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.assigned",
            user_id.has_value() ? "user" : "automated_service",
            user_id.has_value() ? std::to_string(*user_id) : std::to_string(*service_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        if (user_id.has_value()) {
            sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(*user_id);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role assigned successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to assign role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to assign role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRevokeRole(HttpRequestPtr tsp_req) {
    try {
        const auto context = detail::requestContext(tsp_req);
        const std::string& org = context.organisation;
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);
        const int32_t actor_id = context.actor_id;
        const std::string& actor_name = context.actor_name;

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
        sgrn::datastore::services::admin::AdminRepository repository(db);
        const sgrn::datastore::services::admin::RoleTarget target{user_id, service_id};

        // Escalation guard: protect the last active platform administrator
        if (user_id.has_value()) {
            auto last_admin = co_await repository.isLastPlatformAdmin(*user_id);
            if (!last_admin.hasError() && last_admin.value()) {
                co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.revoke_rejected_last_admin",
                    "user", std::to_string(*user_id), tsp_req->getPeerAddr().toIp(), *json, "failed");
                co_return createErrorResponse(
                    "Forbidden: cannot revoke role from the last active platform administrator", k403Forbidden, "AdminApi");
            }
        }

        if (!is_platform) {
            auto role_hit = co_await repository.roleAccess(role_id);
            if (role_hit.hasError() || role_hit.value().organisation != org) {
                co_return createErrorResponse("Role not found in caller's organisation", k404NotFound, "AdminApi");
            }
            if (role_hit.value().system) {
                co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.revoke_rejected_above_level",
                    "role", std::to_string(role_id), tsp_req->getPeerAddr().toIp(), *json, "failed");
                co_return createErrorResponse("Forbidden: cannot revoke system roles (above level)", k403Forbidden, "AdminApi");
            }
            if (user_id.has_value()) {
                auto u_hit = co_await repository.userOrganisation(*user_id);
                if (u_hit.hasError() || u_hit.value().organisation != org) {
                    co_return createErrorResponse("User not found in caller's organisation", k404NotFound, "AdminApi");
                }
            }
            if (service_id.has_value()) {
                auto s_hit = co_await repository.serviceOrganisation(*service_id);
                if (s_hit.hasError() || s_hit.value().organisation != org) {
                    co_return createErrorResponse("Automated service not found in caller's organisation", k404NotFound, "AdminApi");
                }
            }
        }

        auto revoked = co_await repository.revokeRole(target, role_id);
        if (revoked.hasError())
            co_return createErrorResponse(AdminApiError::DbError);

        co_await sgrn::datastore::audit::AuditLogger::log(db, org, "user", actor_id, actor_name, "role.revoked",
            user_id.has_value() ? "user" : "automated_service",
            user_id.has_value() ? std::to_string(*user_id) : std::to_string(*service_id), tsp_req->getPeerAddr().toIp(), *json, "success");

        if (user_id.has_value()) {
            sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(*user_id);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["message"] = "Role revoked successfully";
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to revoke role: {}", e.what());
        co_return createErrorResponse(std::string("Failed to revoke role: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

// GET /api/v1/admin/roles/assignments — who currently holds which role.
// Org-scoped: role org gates the row set, and targets of a foreign org are
// filtered out (defense in depth; assignment writes already enforce this).
Task<HttpResponsePtr> AdminApiHandler::handleListRoleAssignments(HttpRequestPtr tsp_req) {
    try {
        const auto context = detail::requestContext(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT ur.role_id, r.name AS role_name, ur.user_id, ur.automated_service_id, "
            "       u.email AS user_email, a.name AS service_name "
            "FROM core.user_roles ur "
            "JOIN core.roles r ON r.id = ur.role_id "
            "LEFT JOIN core.users u ON u.id = ur.user_id "
            "LEFT JOIN core.automated_services a ON a.id = ur.automated_service_id "
            "WHERE r.organisation = $1 "
            "  AND (ur.user_id IS NULL OR u.organisation = $1) "
            "  AND (ur.automated_service_id IS NULL OR a.organisation = $1) "
            "ORDER BY ur.role_id, ur.user_id NULLS LAST, ur.automated_service_id NULLS LAST",
            {context.organisation});

        Json::Value arr = Json::arrayValue;
        for (const auto& row : res) {
            Json::Value item;
            item["role_id"] = row["role_id"].as<int32_t>();
            item["role_name"] = row["role_name"].as<std::string>();
            if (row["user_id"].isNull()) {
                item["target_type"] = "automated_service";
                item["target_id"] = row["automated_service_id"].as<int32_t>();
                item["target_label"] = row["service_name"].as<std::string>();
            } else {
                item["target_type"] = "user";
                item["target_id"] = row["user_id"].as<int32_t>();
                item["target_label"] = row["user_email"].as<std::string>();
            }
            arr.append(item);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["assignments"] = arr;
        resp["count"] = static_cast<Json::UInt64>(res.size());
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to list role assignments: {}", e.what());
        co_return createErrorResponse(std::string("Failed to list role assignments: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin