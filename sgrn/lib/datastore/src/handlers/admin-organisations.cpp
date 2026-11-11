// admin-organisations.cpp — Organisation lifecycle (platform admin) and
// organisation listing (platform: all; org admin: own row only).
#include "admin-common.hpp"

#include <optional>
#include <string>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace
{
std::optional<int64_t> optionalCap(const Json::Value& t_json, const char* t_key, std::string& o_err) {
    if (!t_json.isMember(t_key) || t_json[t_key].isNull()) {
        return std::nullopt;
    }
    if (!t_json[t_key].isIntegral() || t_json[t_key].asInt64() < 0) {
        o_err = std::string(t_key) + " must be null or a non-negative integer";
        return std::nullopt;
    }
    return t_json[t_key].asInt64();
}

Json::Value organisationRowJson(const drogon::orm::Row& t_row) {
    Json::Value o;
    o["name"] = t_row["name"].as<std::string>();
    o["description"] = t_row["description"].isNull() ? Json::Value(Json::nullValue) : Json::Value(t_row["description"].as<std::string>());
    o["domain"] = t_row["domain"].isNull() ? Json::Value(Json::nullValue) : Json::Value(t_row["domain"].as<std::string>());
    o["status"] = t_row["status"].as<std::string>();
    o["total_virtual_size"] = Json::Int64(t_row["total_virtual_size"].as<int64_t>());
    o["storage_limit"] =
        t_row["storage_limit"].isNull() ? Json::Value(Json::nullValue) : Json::Value(Json::Int64(t_row["storage_limit"].as<int64_t>()));
    o["total_entry_count"] = Json::Int64(t_row["total_entry_count"].as<int64_t>());
    o["entry_count_limit"] = t_row["entry_count_limit"].isNull() ? Json::Value(Json::nullValue)
                                                                 : Json::Value(Json::Int64(t_row["entry_count_limit"].as<int64_t>()));
    o["rate_limit_rpm"] = t_row["rate_limit_rpm"].isNull() ? Json::Value(Json::nullValue) : Json::Value(t_row["rate_limit_rpm"].as<int>());
    return o;
}
} // namespace

Task<HttpResponsePtr> AdminApiHandler::handleListOrganisations(HttpRequestPtr tsp_req) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        static constexpr const char* kSelect =
            "SELECT name, description, domain, status, total_virtual_size, storage_limit, total_entry_count, entry_count_limit, "
            "rate_limit_rpm FROM core.organisations ";
        auto rows = is_platform ? co_await db->execSqlCoro(std::string(kSelect) + "ORDER BY name")
                                : co_await db->execSqlCoro(std::string(kSelect) + "WHERE name = $1", ctx.organisation);

        Json::Value list = Json::arrayValue;
        for (const auto& row : rows) {
            list.append(organisationRowJson(row));
        }

        Json::Value resp;
        resp["success"] = true;
        resp["organisations"] = std::move(list);
        co_return createJsonResponse(resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to list organisations: {}", e.what());
        co_return createErrorResponse(std::string("Failed to list organisations: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleCreateOrganisation(HttpRequestPtr tsp_req) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        auto json = tsp_req->getJsonObject();
        if (!json || !json->isObject()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }

        const std::string name = sgrn::utils::strings::trim(json->get("name", "").asString());
        // Canonical authorities render as "<organisation>-<domain>"; '-' and
        // whitespace would make that ambiguous, on top of the DB's [.@/] ban.
        if (name.empty() || name.size() > 64 || name.find_first_of(".@/- \t") != std::string::npos) {
            co_return createErrorResponse("invalid organisation name", k400BadRequest, "AdminApi");
        }

        std::string cap_err;
        const auto storage_limit = optionalCap(*json, "storage_limit", cap_err);
        const auto entry_limit = optionalCap(*json, "entry_count_limit", cap_err);
        if (!cap_err.empty()) {
            co_return createErrorResponse(cap_err, k400BadRequest, "AdminApi");
        }

        std::optional<std::string> admin_email;
        std::optional<std::string> admin_first;
        std::optional<std::string> admin_family;
        std::optional<std::string> admin_password;
        if (json->isMember("admin") && (*json)["admin"].isObject()) {
            const Json::Value& a = (*json)["admin"];
            admin_email = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(a.get("email", "").asString()));
            admin_first = sgrn::utils::strings::trim(a.get("first_name", "").asString());
            admin_family = sgrn::utils::strings::trim(a.get("family_name", "").asString());
            admin_password = a.get("password", "").asString();

            // Reuse the registration validator (email format, name lengths…).
            Json::Value probe = a;
            probe["organisation"] = name;
            probe["email"] = *admin_email;
            probe["first_name"] = *admin_first;
            probe["family_name"] = *admin_family;
            std::string verr;
            auto payload = deserializeRegisterUserPayload(std::make_shared<Json::Value>(probe), verr);
            if (!payload.has_value()) {
                co_return createErrorResponse(verr, k400BadRequest, "AdminApi");
            }
            if (!admin_password.has_value() || admin_password->size() < 12) {
                co_return createErrorResponse("Initial admin password must be at least 12 characters", k400BadRequest, "AdminApi");
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        std::optional<std::string> description;
        std::optional<std::string> domain;
        if (json->isMember("description") && (*json)["description"].isString()) {
            const auto d = sgrn::utils::strings::trim((*json)["description"].asString());
            description = d.empty() ? std::nullopt : std::optional<std::string>(d);
        }
        if (json->isMember("domain") && (*json)["domain"].isString()) {
            const auto d = sgrn::utils::strings::toLower(sgrn::utils::strings::trim((*json)["domain"].asString()));
            domain = d.empty() ? std::nullopt : std::optional<std::string>(d);
        }

        auto res = co_await db->execSqlCoro("SELECT * FROM core.create_organisation($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)", name,
            description, domain, storage_limit, entry_limit, admin_email, admin_first, admin_family, admin_password, ctx.actor_id);

        Json::Value details(Json::objectValue); // never include admin.password
        details["name"] = name;
        details["storage_limit"] = storage_limit ? Json::Value(Json::Int64(*storage_limit)) : Json::Value(Json::nullValue);
        details["entry_count_limit"] = entry_limit ? Json::Value(Json::Int64(*entry_limit)) : Json::Value(Json::nullValue);
        details["initial_admin_email"] = admin_email ? Json::Value(*admin_email) : Json::Value(Json::nullValue);
        co_await sgrn::datastore::audit::AuditLogger::log(db, name, "user", ctx.actor_id, ctx.actor_name, "organisation.created",
            "organisation", name, tsp_req->getPeerAddr().toIp(), details, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["organisation"] = name;
        if (!res.empty() && !res[0]["out_admin_user_id"].isNull()) {
            resp["admin_user_id"] = res[0]["out_admin_user_id"].as<int32_t>();
        }
        co_return createJsonResponse(resp, k201Created);
    } catch (const drogon::orm::DrogonDbException& e) {
        const std::string msg = e.base().what();
        if (msg.find("duplicate key") != std::string::npos) {
            co_return createErrorResponse("organisation or admin email already exists", k409Conflict, "AdminApi");
        }
        ADMIN_ERROR_LOG("Organisation creation failed: {}", msg);
        co_return createErrorResponse("Organisation creation failed", k400BadRequest, "AdminApi");
    } catch (const std::exception& e) {
        co_return createErrorResponse(std::string("Organisation creation failed: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateOrganisation(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const std::string name = sgrn::utils::strings::trim(t_id);
        auto json = tsp_req->getJsonObject();
        if (name.empty() || !json || !json->isObject()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        auto current = co_await db->execSqlCoro("SELECT description, domain, status FROM core.organisations WHERE name = $1", name);
        if (current.empty()) {
            co_return createErrorResponse("No such organisation", k404NotFound, "AdminApi");
        }

        std::optional<std::string> description;
        std::optional<std::string> domain;
        std::string status = current[0]["status"].as<std::string>();

        if (json->isMember("description")) {
            if (!(*json)["description"].isNull() && !(*json)["description"].isString()) {
                co_return createErrorResponse("description must be a string or null", k400BadRequest, "AdminApi");
            }
            description = (*json)["description"].isNull() ? std::optional<std::string>{}
                                                          : std::optional<std::string>((*json)["description"].asString());
        } else if (!current[0]["description"].isNull()) {
            description = current[0]["description"].as<std::string>();
        }

        if (json->isMember("domain")) {
            if (!(*json)["domain"].isNull() && !(*json)["domain"].isString()) {
                co_return createErrorResponse("domain must be a string or null", k400BadRequest, "AdminApi");
            }
            domain =
                (*json)["domain"].isNull()
                    ? std::optional<std::string>{}
                    : std::optional<std::string>(sgrn::utils::strings::toLower(sgrn::utils::strings::trim((*json)["domain"].asString())));
        } else if (!current[0]["domain"].isNull()) {
            domain = current[0]["domain"].as<std::string>();
        }

        if (json->isMember("status")) {
            if (!(*json)["status"].isString()) {
                co_return createErrorResponse("status must be a string", k400BadRequest, "AdminApi");
            }
            status = (*json)["status"].asString();
        }
        if (status != "active" && status != "suspended" && status != "archived") {
            co_return createErrorResponse("status must be active, suspended or archived", k400BadRequest, "AdminApi");
        }
        if (name == ctx.organisation && status != "active") {
            co_return createErrorResponse("Forbidden: cannot suspend or archive your own organisation", k403Forbidden, "AdminApi");
        }

        co_await db->execSqlCoro(
            "UPDATE core.organisations SET description = $2, domain = $3, status = $4 WHERE name = $1", name, description, domain, status);

        Json::Value details(Json::objectValue);
        if (json->isMember("description")) {
            details["description"] = description ? Json::Value(*description) : Json::Value(Json::nullValue);
        }
        if (json->isMember("domain")) {
            details["domain"] = domain ? Json::Value(*domain) : Json::Value(Json::nullValue);
        }
        if (json->isMember("status")) {
            details["status"] = status;
        }
        co_await sgrn::datastore::audit::AuditLogger::log(db, name, "user", ctx.actor_id, ctx.actor_name, "organisation.updated",
            "organisation", name, tsp_req->getPeerAddr().toIp(), details, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["organisation"] = name;
        co_return createJsonResponse(resp, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return createErrorResponse(std::string("Organisation update failed: ") + e.base().what(), k400BadRequest, "AdminApi");
    } catch (const std::exception& e) {
        co_return createErrorResponse(std::string("Organisation update failed: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
