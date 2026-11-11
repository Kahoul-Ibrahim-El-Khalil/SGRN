// admin-quotas.cpp — Organisation, user, service and domain quotas.
//
// F6: the previous handlers built one UPDATE with every quota column bound
// from a std::optional, so any key absent from the request body was written
// as NULL — sending only `storage_limit` silently lifted every other cap.
// Partial semantics now are:
//   * key absent      -> column untouched
//   * key explicit null -> cap lifted (SET NULL)
//   * key integer     -> validated non-negative, bounded, written
#include "admin-common.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace
{

struct QuotaColumn {
    const char* key;
    const char* pg_type;
    int64_t max_value;
};

// Order matters: the first 3 are valid for organisations/domains, all 6 for
// users and services (applyQuotaUpdate's t_column_count slices the prefix).
constexpr std::array<QuotaColumn, 6> kQuotaColumns = {{
    {"storage_limit", "bigint", INT64_MAX},
    {"entry_count_limit", "bigint", INT64_MAX},
    {"rate_limit_rpm", "int", INT32_MAX},
    {"max_file_size_mb", "bigint", INT64_MAX},
    {"preferred_chunk_size_mb", "int", 1024},
    {"rate_limit_upload_rpm", "int", INT32_MAX},
}};

struct QuotaPatch {
    std::string set_clause;
    std::vector<std::string> binds;
    std::string error;
    Json::Value applied{Json::objectValue};
};

QuotaPatch buildQuotaPatch(const Json::Value& t_json, std::size_t t_column_count) {
    QuotaPatch patch;
    for (std::size_t i = 0; i < t_column_count; ++i) {
        const QuotaColumn& col = kQuotaColumns[i];
        if (!t_json.isMember(col.key)) {
            continue; // absent => untouched
        }
        const Json::Value& v = t_json[col.key];
        if (!patch.set_clause.empty()) {
            patch.set_clause += ", ";
        }
        if (v.isNull()) {
            patch.set_clause += std::string(col.key) + " = NULL"; // explicit null => lift the cap
            patch.applied[col.key] = Json::nullValue;
            continue;
        }
        if (!v.isIntegral() || v.asInt64() < 0 || v.asInt64() > col.max_value) {
            patch.error = std::string(col.key) + " must be null or an integer in [0, " + std::to_string(col.max_value) + "]";
            return patch;
        }
        patch.binds.push_back(std::to_string(v.asInt64()));
        patch.set_clause += fmt::format("{} = ${}::{}", col.key, patch.binds.size(), col.pg_type);
        patch.applied[col.key] = Json::Int64(v.asInt64());
    }
    if (patch.set_clause.empty()) {
        patch.error = "no quota field supplied";
    }
    return patch;
}

// t_key_column is "id" (users/services/domains) or "name" (organisations).
// Scope to the caller's organisation unless platform admin; soft-deleted rows
// are never updated. One audit row per successful update; missing and
// other-tenant targets produce the identical error (no tenant enumeration).
Task<HttpResponsePtr> applyQuotaUpdate(HttpRequestPtr tsp_req, const char* t_table, const char* t_key_column, std::string t_key_value,
    bool t_org_scoped, bool t_soft_deletable, const char* t_target_type, std::size_t t_column_count) {
    const auto ctx = detail::requestContext(tsp_req);
    auto json = tsp_req->getJsonObject();
    if (!json || !json->isObject()) {
        co_return createErrorResponse(AdminApiError::InvalidPayload);
    }
    QuotaPatch patch = buildQuotaPatch(*json, t_column_count);
    if (!patch.error.empty()) {
        co_return createErrorResponse(patch.error, k400BadRequest, "AdminApi");
    }

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db = db_res.value();

    std::vector<std::string> binds = std::move(patch.binds);
    binds.push_back(t_key_value);
    std::string sql = fmt::format("UPDATE {} SET {} WHERE {} = ${}", t_table, patch.set_clause, t_key_column, binds.size());
    if (t_soft_deletable) {
        sql += " AND deleted_at IS NULL";
    }
    if (t_org_scoped && !sgrn::datastore::isPlatformAdmin(tsp_req)) {
        binds.push_back(ctx.organisation);
        sql += fmt::format(" AND organisation = ${}", binds.size());
    }

    try {
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, binds);
        if (res.affectedRows() == 0) {
            // Same answer for "missing" and "belongs to another tenant".
            co_return createErrorResponse(AdminApiError::InvalidUserId);
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return createErrorResponse(std::string("Quota update failed: ") + e.base().what(), k400BadRequest, "AdminApi");
    }

    co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "quota.updated",
        t_target_type, t_key_value, tsp_req->getPeerAddr().toIp(), patch.applied, "success");

    Json::Value resp;
    resp["success"] = true;
    resp["applied"] = patch.applied;
    co_return createJsonResponse(resp, k200OK);
}

} // namespace

Task<HttpResponsePtr> AdminApiHandler::handleUpdateOrgQuota(HttpRequestPtr tsp_req) {
    const auto ctx = detail::requestContext(tsp_req);
    std::string target = ctx.organisation;
    if (sgrn::datastore::isPlatformAdmin(tsp_req)) { // platform admin may name another org via ?name=
        const std::string named = tsp_req->getParameter("name");
        if (!named.empty()) {
            target = named;
        }
    }
    co_return co_await applyQuotaUpdate(tsp_req, "core.organisations", "name", target, false, false, "organisation", 3);
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateUserQuota(HttpRequestPtr tsp_req, std::string t_id) {
    co_return co_await applyQuotaUpdate(tsp_req, "core.users", "id", std::move(t_id), true, true, "user", kQuotaColumns.size());
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateServiceQuota(HttpRequestPtr tsp_req, std::string t_id) {
    co_return co_await applyQuotaUpdate(
        tsp_req, "core.automated_services", "id", std::move(t_id), true, true, "automated_service", kQuotaColumns.size());
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateDomainQuota(HttpRequestPtr tsp_req, std::string t_id) {
    co_return co_await applyQuotaUpdate(tsp_req, "core.domains", "id", std::move(t_id), true, false, "domain", 2);
}

} // namespace sgrn::datastore::handlers::admin
