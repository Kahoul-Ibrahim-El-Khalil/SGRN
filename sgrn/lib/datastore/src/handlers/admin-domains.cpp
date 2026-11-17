// admin-domains.cpp — hand-written domain lifecycle routes.
//
// `name` and `parent_id` are deliberately non-updatable in generated CRUD:
// a plain PATCH re-parents or renames one row while leaving descendants'
// paths stale, and core.admin_level() matches domain admins by path prefix.
// These routes go through the subtree-safe SQL functions instead.
#include "admin-common.hpp"

#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/utils/json-serialization.hpp>

#include <string>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{
struct DomainOrganisationRow {
    std::string organisation;
};

struct RenameDomainRequest {
    std::string name;
};

struct MoveDomainRequest {
    std::optional<int32_t> parent_id;
};

struct RenameDomainResponse {
    int32_t domain_id{0};
    std::string name;
};

struct MoveDomainResponse {
    int32_t domain_id{0};
    std::optional<int32_t> parent_id;
};

struct RenameDomainAuditDetails {
    std::string name;
};

struct MoveDomainAuditDetails {
    std::optional<int32_t> parent_id;
};

// "missing" and "other tenant" must be indistinguishable.
Task<std::optional<std::string>> domainOrganisation(
    const drogon::orm::DbClientPtr& t_db, int32_t t_domain_id, const std::string& t_caller_org, bool t_is_platform) {
    static constexpr char kDomainOrganisationQuery[] = "SELECT organisation FROM core.domains WHERE id = $1";
    auto rows = co_await t_db->execSqlCoro(kDomainOrganisationQuery, t_domain_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const std::string org = sgrn::orm::fromRow<DomainOrganisationRow, kDomainOrganisationQuery>(rows[0]).organisation;
    if (!t_is_platform && org != t_caller_org) {
        co_return std::nullopt;
    }
    co_return org;
}

HttpResponsePtr sqlErrorToResponse(const drogon::orm::DrogonDbException& t_err, const char* t_what) {
    const std::string msg = t_err.base().what();
    if (msg.find("already exists") != std::string::npos || msg.find("duplicate key") != std::string::npos ||
        msg.find("contains files") != std::string::npos || msg.find("descendant") != std::string::npos ||
        msg.find("Cannot move") != std::string::npos) {
        return createTypedResponse(BackendErrorResponse{msg, k409Conflict, "AdminApi"});
    }
    if (msg.find("lower-case") != std::string::npos || msg.find("must not contain") != std::string::npos ||
        msg.find("must not be empty") != std::string::npos) {
        return createTypedResponse(BackendErrorResponse{msg, k400BadRequest, "AdminApi"});
    }
    if (msg.find("does not exist") != std::string::npos) {
        return createTypedResponse(BackendErrorResponse{msg, k404NotFound, "AdminApi"});
    }
    return createTypedResponse(BackendErrorResponse{std::string(t_what) + ": " + msg, k400BadRequest, "AdminApi"});
}
} // namespace detail
using namespace detail;

Task<HttpResponsePtr> AdminApiHandler::handleRenameDomain(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t domain_id = 0;
        try {
            domain_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        auto body_res = parseTypedRequest<RenameDomainRequest>(tsp_req);
        if (body_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const std::string new_name = sgrn::utils::strings::trim(body_res->name);
        if (new_name.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"name must not be empty", k400BadRequest, "AdminApi"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        const auto org = co_await domainOrganisation(db, domain_id, ctx.organisation, is_platform);
        if (!org.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        co_await db->execSqlCoro("SELECT core.rename_domain($1, $2)", domain_id, new_name);
        // Domain paths moved: any cached domain-scope level may now be stale.
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateAll();

        co_await sgrn::datastore::audit::AuditLogger::log(db, *org, "user", ctx.actor_id, ctx.actor_name, "domain.renamed", "domain",
            std::to_string(domain_id), tsp_req->getPeerAddr().toIp(), RenameDomainAuditDetails{new_name}, "success");

        co_return createTypedResponse(tsp_req, RenameDomainResponse{domain_id, new_name}, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sqlErrorToResponse(e, "Domain rename failed");
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Domain rename failed: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{std::string("Domain rename failed: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleMoveDomain(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t domain_id = 0;
        try {
            domain_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }

        auto body_res = parseTypedRequest<MoveDomainRequest>(tsp_req);
        if (body_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
        }
        const auto& parent_id = body_res->parent_id;
        if (parent_id.has_value() && *parent_id <= 0) {
            co_return createTypedResponse(BackendErrorResponse{"parent_id must be null or a positive integer", k400BadRequest, "AdminApi"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        const auto org = co_await domainOrganisation(db, domain_id, ctx.organisation, is_platform);
        if (!org.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
        if (parent_id.has_value()) {
            // Same-org check up front so the caller gets the same answer for
            // "missing" and "other tenant" parent domains.
            const auto parent_org = co_await domainOrganisation(db, *parent_id, ctx.organisation, is_platform);
            if (!parent_org.has_value() || *parent_org != *org) {
                co_return createTypedResponse(
                    BackendErrorResponse{"parent domain must belong to the same organisation", k409Conflict, "AdminApi"});
            }
        }

        co_await db->execSqlCoro("SELECT core.move_domain_subtree($1, $2)", domain_id, parent_id);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateAll();

        co_await sgrn::datastore::audit::AuditLogger::log(db, *org, "user", ctx.actor_id, ctx.actor_name, "domain.moved", "domain",
            std::to_string(domain_id), tsp_req->getPeerAddr().toIp(), MoveDomainAuditDetails{parent_id}, "success");

        co_return createTypedResponse(tsp_req, MoveDomainResponse{domain_id, parent_id}, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sqlErrorToResponse(e, "Domain move failed");
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Domain move failed: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{std::string("Domain move failed: ") + e.what(), k400BadRequest, "AdminApi"});
    }
}

} // namespace sgrn::datastore::handlers::admin
