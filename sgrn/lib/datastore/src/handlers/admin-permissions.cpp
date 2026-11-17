// admin-permissions.cpp — User permission administration.
#include "admin-common.hpp"

#include <sgrn/datastore/core/orm-glaze.hpp>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{
static constexpr char kUserByIdQuery[] = "SELECT email, organisation FROM core.users WHERE id = $1 AND deleted_at IS NULL";
static constexpr char kUserByEmailQuery[] = "SELECT id, organisation FROM core.users WHERE email = $1 AND deleted_at IS NULL";
static constexpr char kPermissionCountQuery[] = "SELECT COUNT(*) AS count FROM core.user_domain_permissions WHERE user_id = $1";
static constexpr char kUpsertPermissionQuery[] =
    "INSERT INTO core.user_domain_permissions (user_id, organisation, domain, allowed_subpath, can_read, can_write, can_delete) "
    "VALUES ($1, $2, $3, $4, $5, $6, $7) "
    "ON CONFLICT (user_id, organisation, domain) DO UPDATE SET allowed_subpath = EXCLUDED.allowed_subpath, "
    "can_read = EXCLUDED.can_read, can_write = EXCLUDED.can_write, can_delete = EXCLUDED.can_delete, "
    "updated_at = now() "
    "RETURNING id, allowed_subpath, can_read, can_write, can_delete";

struct GrantPermissionRequest {
    std::optional<int32_t> user_id;
    std::optional<std::string> email;
    std::string domain;
    std::optional<std::string> organisation;
    std::string allowed_subpath{"/"};
    bool can_read{true};
    bool can_write{true};
    bool can_delete{false};
};

struct PermissionRow {
    int64_t id{0};
    int32_t user_id{0};
    std::string email;
    std::string organisation;
    std::string domain;
    std::string allowed_subpath;
    bool can_read{false};
    bool can_write{false};
    bool can_delete{false};
};

struct PermissionListResponse {
    std::vector<PermissionRow> permissions;
    int64_t count{0};
};
struct PermissionGrantResponse {
    int64_t id{0};
    int32_t user_id{0};
    std::string email;
    std::string organisation;
    std::string domain;
    std::string allowed_subpath;
    bool can_read{false};
    bool can_write{false};
    bool can_delete{false};
    std::optional<std::string> note;
};

struct PermissionGrantRow {
    int64_t id{0};
    std::string allowed_subpath;
    bool can_read{false};
    bool can_write{false};
    bool can_delete{false};
};

struct PermissionRevokeResponse {
    std::string revoked;
    std::optional<std::string> warning;
};

struct UserEmailOrganisationRow {
    std::string email;
    std::string organisation;
};

struct UserIdOrganisationRow {
    int32_t id{0};
    std::string organisation;
};

struct OrganisationRow {
    std::string organisation;
};

struct UserIdRow {
    int32_t user_id{0};
};

struct CountRow {
    std::string count;
};
} // namespace detail
using namespace detail;

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
                co_return createTypedResponse(
                    BackendErrorResponse{"Forbidden: cannot view permissions of another organisation", k403Forbidden, "AdminApi"});
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
                co_return createTypedResponse(BackendErrorResponse{"user_id must be an integer", k400BadRequest, "AdminApi"});
            }
            if (user_id <= 0) {
                co_return createTypedResponse(BackendErrorResponse{"user_id must be positive", k400BadRequest, "AdminApi"});
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // Filters are ANDed; each absent filter matches everything. LIKE
        // patterns go through parameters (no string splicing of values).
        static constexpr char kQuery[] =
            "SELECT p.id, p.user_id, u.email, p.organisation, p.domain, p.allowed_subpath, p.can_read, p.can_write, p.can_delete "
            "FROM core.user_domain_permissions p JOIN core.users u ON u.id = p.user_id "
            "WHERE u.deleted_at IS NULL "
            "AND ($1 = 0 OR p.user_id = $1) "
            "AND ($2 = '' OR u.email ILIKE '%' || $2 || '%') "
            "AND ($3 = '' OR p.domain = $3) "
            "AND ($4 = '' OR p.organisation = $4) "
            "ORDER BY u.email, p.domain LIMIT $5";

        auto rows = co_await db->execSqlCoro(kQuery, user_id, email, domain, organisation, static_cast<int64_t>(limit));

        const PermissionListResponse out{
            .permissions = sgrn::orm::fromResult<PermissionRow, kQuery>(rows),
            .count = static_cast<int64_t>(rows.size()),
        };
        co_return createTypedResponse(tsp_req, out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Permission list DB error: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Permission list error: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to list permissions: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleGrantPermission(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<GrantPermissionRequest>(tsp_req);
    if (req_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
    }
    const auto& req = req_res.value();

    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        // Resolve caller session user ID for self-grant prevention
        int32_t actor_id = 0;
        std::string actor_name;
        if (tsp_req->attributes()->find("auth_context")) {
            const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
            actor_id = auth.user.id;
            actor_name = auth.user.email;
        } else if (tsp_req->attributes()->find("session_claims")) {
            const auto& user = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user;
            actor_id = user.id;
            actor_name = user.email;
        }

        // Escalation guard: self-grant is strictly forbidden (checked before DB dependency)
        if ((actor_id > 0 && req.user_id.has_value() && *req.user_id == actor_id) ||
            (!actor_name.empty() && req.email.has_value() && sgrn::utils::strings::trim(*req.email) == actor_name)) {
            auto db_res = sgrn::datastore::core::getDbClient();
            if (!db_res.hasError()) {
                co_await sgrn::datastore::audit::AuditLogger::log(db_res.value(), session_org, "user", actor_id, actor_name,
                    "permission.grant_rejected_self_grant", "user", std::to_string(actor_id), tsp_req->getPeerAddr().toIp(), req, "failed");
            }
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot grant permissions to yourself (self-grant prohibited)", k403Forbidden, "AdminApi"});
        }

        // Resolve the user by id or email (must exist and not be soft-deleted).
        int32_t user_id = 0;
        std::string email;
        std::string user_org;
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        if (req.user_id.has_value() && *req.user_id > 0) {
            user_id = *req.user_id;
            auto hit = co_await db->execSqlCoro(kUserByIdQuery, user_id);
            if (hit.empty()) {
                co_return createTypedResponse(BackendErrorResponse{"No such user (or soft-deleted)", k404NotFound, "AdminApi"});
            }
            const auto row = sgrn::orm::fromRow<UserEmailOrganisationRow, kUserByIdQuery>(hit[0]);
            email = row.email;
            user_org = row.organisation;
        } else if (req.email.has_value() && !req.email->empty()) {
            email = sgrn::utils::strings::trim(*req.email);
            auto hit = co_await db->execSqlCoro(kUserByEmailQuery, email);
            if (hit.empty()) {
                co_return createTypedResponse(BackendErrorResponse{"No such user (or soft-deleted)", k404NotFound, "AdminApi"});
            }
            const auto row = sgrn::orm::fromRow<UserIdOrganisationRow, kUserByEmailQuery>(hit[0]);
            user_id = row.id;
            user_org = row.organisation;
        } else {
            co_return createTypedResponse(BackendErrorResponse{"user_id or email is required", k400BadRequest, "AdminApi"});
        }

        if (!is_platform && user_org != session_org) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot grant permissions to users in another organisation", k403Forbidden, "AdminApi"});
        }

        if (req.domain.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"domain is required", k400BadRequest, "AdminApi"});
        }
        const std::string domain = sgrn::utils::strings::trim(req.domain);

        // Organisation defaults to the user's own; cross-org grants are
        // refused (a user belongs to exactly one organisation, and the
        // enforcement lookup is domain-scoped within it).
        std::string organisation = user_org;
        if (req.organisation.has_value() && !req.organisation->empty()) {
            organisation = sgrn::utils::strings::trim(*req.organisation);
            if (organisation != user_org) {
                co_return createTypedResponse(
                    BackendErrorResponse{"organisation must match the user's own organisation", k400BadRequest, "AdminApi"});
            }
        }
        if (!is_platform && organisation != session_org) {
            co_return createTypedResponse(
                BackendErrorResponse{"Forbidden: cannot grant permissions for another organisation", k403Forbidden, "AdminApi"});
        }

        // The FK would 500 on an unknown domain — pre-check for a clean 400.
        auto dom = co_await db->execSqlCoro("SELECT 1 FROM core.domains WHERE organisation = $1 AND name = $2", organisation, domain);
        if (dom.empty()) {
            co_return createTypedResponse(
                BackendErrorResponse{"Unknown domain '" + domain + "' in organisation '" + organisation + "'", k400BadRequest, "AdminApi"});
        }

        std::string subpath = req.allowed_subpath.empty() ? "/" : sgrn::utils::strings::trim(req.allowed_subpath);
        if (subpath.empty()) {
            subpath = "/";
        }
        if (subpath.front() != '/') {
            co_return createTypedResponse(BackendErrorResponse{"allowed_subpath must start with '/'", k400BadRequest, "AdminApi"});
        }
        const bool can_read = req.can_read;
        const bool can_write = req.can_write;
        const bool can_delete = req.can_delete;

        auto before = co_await db->execSqlCoro(kPermissionCountQuery, user_id);
        const bool first_grant = !before.empty() && sgrn::orm::fromRow<CountRow, kPermissionCountQuery>(before[0]).count == "0";

        auto upserted =
            co_await db->execSqlCoro(kUpsertPermissionQuery, user_id, organisation, domain, subpath, can_read, can_write, can_delete);
        const auto row = sgrn::orm::fromRowByName<PermissionGrantRow>(upserted[0]);
        PermissionGrantResponse out{
            .id = row.id,
            .user_id = user_id,
            .email = email,
            .organisation = organisation,
            .domain = domain,
            .allowed_subpath = row.allowed_subpath,
            .can_read = row.can_read,
            .can_write = row.can_write,
            .can_delete = row.can_delete,
            .note = std::nullopt,
        };
        if (first_grant) {
            out.note = "First domain grant for this user — storage access unlocked (zero-trust default was deny-all).";
        }
        co_await sgrn::datastore::audit::AuditLogger::log(db, organisation, "user", actor_id, actor_name, "permission.granted", "user",
            std::to_string(user_id), tsp_req->getPeerAddr().toIp(), req, "success");
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(user_id);
        co_return createTypedResponse(tsp_req, out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Permission grant DB error: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Permission grant error: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to grant permission: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleRevokePermission(HttpRequestPtr tsp_req) {
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
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
                co_return createTypedResponse(BackendErrorResponse{"id must be an integer", k400BadRequest, "AdminApi"});
            }
            if (!is_platform) {
                auto hit = co_await db->execSqlCoro("SELECT organisation FROM core.user_domain_permissions WHERE id = $1", id);
                if (hit.empty() || sgrn::orm::fromRowByName<OrganisationRow>(hit[0]).organisation != session_org) {
                    co_return createTypedResponse(BackendErrorResponse{"No such permission grant", k404NotFound, "AdminApi"});
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
                co_return createTypedResponse(
                    BackendErrorResponse{"supply ?id= or ?user_id=&organisation=&domain=", k400BadRequest, "AdminApi"});
            }
            if (user_id <= 0 || domain.empty()) {
                co_return createTypedResponse(
                    BackendErrorResponse{"supply ?id= or ?user_id=&organisation=&domain=", k400BadRequest, "AdminApi"});
            }
            if (!is_platform) {
                if (!organisation.empty() && organisation != session_org) {
                    co_return createTypedResponse(
                        BackendErrorResponse{"Forbidden: cannot revoke permissions for another organisation", k403Forbidden, "AdminApi"});
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
            co_return createTypedResponse(BackendErrorResponse{"No such permission grant", k404NotFound, "AdminApi"});
        }
        const int32_t affected_user = sgrn::orm::fromRowByName<UserIdRow>(deleted[0]).user_id;
        auto remaining =
            co_await db->execSqlCoro("SELECT COUNT(*) AS count FROM core.user_domain_permissions WHERE user_id = $1", affected_user);

        PermissionRevokeResponse out;
        out.revoked = id_desc;
        if (!remaining.empty() && sgrn::orm::fromRowByName<CountRow>(remaining[0]).count == "0") {
            out.warning = "User now holds no domain grants — zero-trust default-deny locks them out of storage entirely.";
        }
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateUser(affected_user);
        co_return createTypedResponse(tsp_req, out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        ADMIN_ERROR_LOG("Permission revoke DB error: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::DbError});
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Permission revoke error: {}", e.what());
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Failed to revoke permission: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

} // namespace sgrn::datastore::handlers::admin
