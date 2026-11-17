// admin-organisations.cpp — Organisation lifecycle (platform admin) and
// organisation listing (platform: all; org admin: own row only).
#include "admin-common.hpp"

#include <sgrn/datastore/core/orm-glaze.hpp>
#include <optional>
#include <string>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{
struct CreateOrganisationAdminPayload {
    std::string email;
    std::string first_name;
    std::string family_name;
    std::string password;
};

struct CreateOrganisationRequest {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> domain;
    std::optional<int64_t> storage_limit;
    std::optional<int64_t> entry_count_limit;
    std::optional<CreateOrganisationAdminPayload> admin;
};

struct UpdateOrganisationRequest {
    std::optional<std::string> description;
    std::optional<std::string> domain;
    std::optional<std::string> status;
    std::optional<int64_t> storage_limit;
    std::optional<int64_t> entry_count_limit;
    std::optional<int32_t> rate_limit_rpm;
};

struct OrganisationRow {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> domain;
    std::string status;
    int64_t total_virtual_size{0};
    std::optional<int64_t> storage_limit;
    int64_t total_entry_count{0};
    std::optional<int64_t> entry_count_limit;
    std::optional<int32_t> rate_limit_rpm;
};

struct CurrentOrganisation {
    std::optional<std::string> description;
    std::optional<std::string> domain;
    std::string status;
};

struct OrganisationListResponse {
    std::vector<OrganisationRow> organisations;
};
struct OrganisationCreatedResponse {
    std::string organisation;
};
struct OrganisationCreatedWithAdminResponse {
    std::string organisation;
    int32_t admin_user_id{0};
};
struct OrganisationUpdatedResponse {
    std::string organisation;
};

struct OrganisationAdminIdRow {
    int32_t out_admin_user_id{0};
};

inline std::optional<int64_t> optionalCap(const Json::Value& t_json, const char* t_key, std::string& o_err) {
    if (!t_json.isMember(t_key) || t_json[t_key].isNull()) {
        return std::nullopt;
    }
    if (!t_json[t_key].isIntegral() || t_json[t_key].asInt64() < 0) {
        o_err = std::string(t_key) + " must be null or a non-negative integer";
        return std::nullopt;
    }
    return t_json[t_key].asInt64();
}
} // namespace detail

Task<HttpResponsePtr> AdminApiHandler::handleListOrganisations(HttpRequestPtr tsp_req) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        static constexpr char kSelect[] =
            "SELECT name, description, domain, status, total_virtual_size, storage_limit, total_entry_count, entry_count_limit, "
            "rate_limit_rpm FROM core.organisations ";
        auto rows = is_platform ? co_await db->execSqlCoro(std::string(kSelect) + "ORDER BY name")
                                : co_await db->execSqlCoro(std::string(kSelect) + "WHERE name = $1", ctx.organisation);

        const detail::OrganisationListResponse resp{
            .organisations = sgrn::orm::fromResult<detail::OrganisationRow, kSelect>(rows),
        };
        co_return createTypedResponse(tsp_req, resp, k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to list organisations: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to list organisations: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleCreateOrganisation(HttpRequestPtr tsp_req) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        auto req_res = parseTypedRequest<detail::CreateOrganisationRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const auto& req = req_res.value();

        const std::string name = sgrn::utils::strings::trim(req.name);
        // Canonical authorities render as "<organisation>-<domain>"; '-' and
        // whitespace would make that ambiguous, on top of the DB's [.@/] ban.
        if (name.empty() || name.size() > 64 || name.find_first_of(".@/- \t") != std::string::npos) {
            co_return createTypedResponse(BackendErrorResponse{"invalid organisation name", k400BadRequest, "AdminApi"});
        }

        if (req.storage_limit.has_value() && *req.storage_limit < 0) {
            co_return createTypedResponse(
                BackendErrorResponse{"storage_limit must be null or a non-negative integer", k400BadRequest, "AdminApi"});
        }
        if (req.entry_count_limit.has_value() && *req.entry_count_limit < 0) {
            co_return createTypedResponse(
                BackendErrorResponse{"entry_count_limit must be null or a non-negative integer", k400BadRequest, "AdminApi"});
        }

        const auto storage_limit = req.storage_limit;
        const auto entry_limit = req.entry_count_limit;

        std::optional<std::string> admin_email;
        std::optional<std::string> admin_first;
        std::optional<std::string> admin_family;
        std::optional<std::string> admin_password;
        if (req.admin.has_value()) {
            const auto& a = *req.admin;
            admin_email = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(a.email));
            admin_first = sgrn::utils::strings::trim(a.first_name);
            admin_family = sgrn::utils::strings::trim(a.family_name);
            admin_password = a.password;

            // Reuse the registration validator (email format, name lengths…).
            RegisterUserPayload probe;
            probe.organisation = name;
            probe.email = *admin_email;
            probe.first_name = *admin_first;
            probe.family_name = *admin_family;
            probe.password = *admin_password;
            probe.status = "active";
            std::string verr;
            if (!validateRegisterUserPayload(probe, verr)) {
                co_return createTypedResponse(BackendErrorResponse{verr, k400BadRequest, "AdminApi"});
            }
            if (!admin_password.has_value() || admin_password->size() < 12) {
                co_return createTypedResponse(
                    BackendErrorResponse{"Initial admin password must be at least 12 characters", k400BadRequest, "AdminApi"});
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        std::optional<std::string> description;
        std::optional<std::string> domain;
        if (req.description.has_value()) {
            const auto d = sgrn::utils::strings::trim(*req.description);
            description = d.empty() ? std::nullopt : std::optional<std::string>(d);
        }
        if (req.domain.has_value()) {
            const auto d = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(*req.domain));
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

        if (!res.empty() && !res[0]["out_admin_user_id"].isNull()) {
            const auto row = sgrn::orm::fromRowByName<detail::OrganisationAdminIdRow>(res[0]);
            co_return createTypedResponse(tsp_req, detail::OrganisationCreatedWithAdminResponse{name, row.out_admin_user_id}, k201Created);
        }
        co_return createTypedResponse(tsp_req, detail::OrganisationCreatedResponse{name}, k201Created);
    } catch (const drogon::orm::DrogonDbException& e) {
        const std::string msg = e.base().what();
        if (msg.find("duplicate key") != std::string::npos) {
            co_return createTypedResponse(BackendErrorResponse{"organisation or admin email already exists", k409Conflict, "AdminApi"});
        }
        ADMIN_ERROR_LOG("Organisation creation failed: {}", msg);
        co_return createTypedResponse(BackendErrorResponse{"Organisation creation failed", k400BadRequest, "AdminApi"});
    } catch (const std::exception& e) {
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Organisation creation failed: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateOrganisation(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const std::string name = sgrn::utils::strings::trim(t_id);
        auto req_res = parseTypedRequest<detail::UpdateOrganisationRequest>(tsp_req);
        if (name.empty() || req_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const auto& req = req_res.value();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        static constexpr char kCurrentOrganisationQuery[] = "SELECT description, domain, status FROM core.organisations WHERE name = $1";
        auto current = co_await db->execSqlCoro(kCurrentOrganisationQuery, name);
        if (current.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"No such organisation", k404NotFound, "AdminApi"});
        }

        const auto current_organisation = sgrn::orm::fromRow<detail::CurrentOrganisation, kCurrentOrganisationQuery>(current[0]);
        std::optional<std::string> description;
        std::optional<std::string> domain;
        std::string status = current_organisation.status;

        if (req.description.has_value()) {
            description = req.description;
        } else {
            description = current_organisation.description;
        }

        if (req.domain.has_value()) {
            domain = sgrn::utils::strings::toLower(sgrn::utils::strings::trim(*req.domain));
        } else {
            domain = current_organisation.domain;
        }

        if (req.status.has_value()) {
            status = *req.status;
        }
        if (status != "active" && status != "suspended" && status != "archived") {
            co_return createTypedResponse(BackendErrorResponse{"status must be active, suspended or archived", k400BadRequest, "AdminApi"});
        }
        if (name == ctx.organisation && status != "active") {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot suspend or archive your own organisation", k403Forbidden, "AdminApi"});
        }

        co_await db->execSqlCoro(
            "UPDATE core.organisations SET description = $2, domain = $3, status = $4 WHERE name = $1", name, description, domain, status);

        co_await sgrn::datastore::audit::AuditLogger::log(db, name, "user", ctx.actor_id, ctx.actor_name, "organisation.updated",
            "organisation", name, tsp_req->getPeerAddr().toIp(), req, "success");

        co_return createTypedResponse(tsp_req, detail::OrganisationUpdatedResponse{name}, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Organisation update failed: ") + e.base().what(), k400BadRequest, "AdminApi"});
    } catch (const std::exception& e) {
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Organisation update failed: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

} // namespace sgrn::datastore::handlers::admin
