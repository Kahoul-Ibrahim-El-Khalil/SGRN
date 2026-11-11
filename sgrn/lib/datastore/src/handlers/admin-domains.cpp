// admin-domains.cpp — hand-written domain lifecycle routes.
//
// `name` and `parent_id` are deliberately non-updatable in generated CRUD:
// a plain PATCH re-parents or renames one row while leaving descendants'
// paths stale, and core.admin_level() matches domain admins by path prefix.
// These routes go through the subtree-safe SQL functions instead.
#include "admin-common.hpp"

#include <sgrn/utils/json-serialization.hpp>

#include <string>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace
{
struct RenameDomainResponse {
    bool success{true};
    int32_t domain_id{0};
    std::string name;
};

struct MoveDomainResponse {
    bool success{true};
    int32_t domain_id{0};
    std::optional<int32_t> parent_id;
};

struct RenameDomainAuditDetails {
    std::string name;
};

struct MoveDomainAuditDetails {
    std::optional<int32_t> parent_id;
};

template <typename T>
Task<HttpResponsePtr> jsonResponse(T t_value) {
    auto response_result = sgrn::json::write(t_value);
    if (response_result.hasError()) {
        co_return createErrorResponse("Failed to serialize domain response", k500InternalServerError, "AdminApi");
    }
    std::string response_json = std::move(response_result).value();
    co_return createJsonResponse(std::move(response_json), k200OK);
}

// "missing" and "other tenant" must be indistinguishable.
Task<std::optional<std::string>> domainOrganisation(
    const drogon::orm::DbClientPtr& t_db, int32_t t_domain_id, const std::string& t_caller_org, bool t_is_platform) {
    auto rows = co_await t_db->execSqlCoro("SELECT organisation FROM core.domains WHERE id = $1", t_domain_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const std::string org = rows[0]["organisation"].as<std::string>();
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
        return createErrorResponse(msg, k409Conflict, "AdminApi");
    }
    if (msg.find("lower-case") != std::string::npos || msg.find("must not contain") != std::string::npos ||
        msg.find("must not be empty") != std::string::npos) {
        return createErrorResponse(msg, k400BadRequest, "AdminApi");
    }
    if (msg.find("does not exist") != std::string::npos) {
        return createErrorResponse(msg, k404NotFound, "AdminApi");
    }
    return createErrorResponse(std::string(t_what) + ": " + msg, k400BadRequest, "AdminApi");
}
} // namespace

Task<HttpResponsePtr> AdminApiHandler::handleRenameDomain(HttpRequestPtr tsp_req, std::string t_id) {
    try {
        const auto ctx = detail::requestContext(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        int32_t domain_id = 0;
        try {
            domain_id = std::stoi(t_id);
        } catch (const std::exception&) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("name") || !(*json)["name"].isString()) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }
        const std::string new_name = sgrn::utils::strings::trim((*json)["name"].asString());
        if (new_name.empty()) {
            co_return createErrorResponse("name must not be empty", k400BadRequest, "AdminApi");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        const auto org = co_await domainOrganisation(db, domain_id, ctx.organisation, is_platform);
        if (!org.has_value()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        co_await db->execSqlCoro("SELECT core.rename_domain($1, $2)", domain_id, new_name);
        // Domain paths moved: any cached domain-scope level may now be stale.
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateAll();

        co_await sgrn::datastore::audit::AuditLogger::log(db, *org, "user", ctx.actor_id, ctx.actor_name, "domain.renamed", "domain",
            std::to_string(domain_id), tsp_req->getPeerAddr().toIp(), RenameDomainAuditDetails{new_name}, "success");

        co_return co_await jsonResponse(RenameDomainResponse{true, domain_id, new_name});
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sqlErrorToResponse(e, "Domain rename failed");
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Domain rename failed: {}", e.what());
        co_return createErrorResponse(std::string("Domain rename failed: ") + e.what(), k400BadRequest, "AdminApi");
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
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("parent_id")) {
            co_return createErrorResponse(AdminApiError::InvalidPayload);
        }
        std::optional<int32_t> parent_id;
        if (!(*json)["parent_id"].isNull()) {
            if (!(*json)["parent_id"].isInt() || (*json)["parent_id"].asInt() <= 0) {
                co_return createErrorResponse("parent_id must be null or a positive integer", k400BadRequest, "AdminApi");
            }
            parent_id = (*json)["parent_id"].asInt();
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        const auto org = co_await domainOrganisation(db, domain_id, ctx.organisation, is_platform);
        if (!org.has_value()) {
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
        if (parent_id.has_value()) {
            // Same-org check up front so the caller gets the same answer for
            // "missing" and "other tenant" parent domains.
            const auto parent_org = co_await domainOrganisation(db, *parent_id, ctx.organisation, is_platform);
            if (!parent_org.has_value() || *parent_org != *org) {
                co_return createErrorResponse("parent domain must belong to the same organisation", k409Conflict, "AdminApi");
            }
        }

        co_await db->execSqlCoro("SELECT core.move_domain_subtree($1, $2)", domain_id, parent_id);
        sgrn::datastore::filters::AdminLevelCache::instance().invalidateAll();

        co_await sgrn::datastore::audit::AuditLogger::log(db, *org, "user", ctx.actor_id, ctx.actor_name, "domain.moved", "domain",
            std::to_string(domain_id), tsp_req->getPeerAddr().toIp(), MoveDomainAuditDetails{parent_id}, "success");

        co_return co_await jsonResponse(MoveDomainResponse{true, domain_id, parent_id});
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sqlErrorToResponse(e, "Domain move failed");
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Domain move failed: {}", e.what());
        co_return createErrorResponse(std::string("Domain move failed: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
