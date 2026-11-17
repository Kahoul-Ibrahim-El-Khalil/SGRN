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

namespace detail
{

struct QuotaUpdateRequest {
    std::optional<std::optional<int64_t>> storage_limit;
    std::optional<std::optional<int64_t>> entry_count_limit;
    std::optional<std::optional<int32_t>> rate_limit_rpm;
    std::optional<std::optional<int64_t>> max_file_size_mb;
    std::optional<std::optional<int32_t>> preferred_chunk_size_mb;
    std::optional<std::optional<int32_t>> rate_limit_upload_rpm;
};

struct QuotaUpdateResponse {
    sgrn::json::RawJson applied{"{}"};
};

struct QuotaPatch {
    std::string set_clause;
    std::vector<std::string> binds;
    std::string error;
    std::string applied_json{"{}"};
};

QuotaPatch buildQuotaPatch(const QuotaUpdateRequest& req, std::size_t t_column_count) {
    QuotaPatch patch;
    std::vector<std::string> applied_fields;

    auto check_field = [&](std::size_t col_idx, const char* name, const char* pg_type, int64_t max_val, auto& opt) {
        if (col_idx >= t_column_count || !opt.has_value()) {
            return true;
        }
        if (!patch.set_clause.empty()) {
            patch.set_clause += ", ";
        }
        if (!opt->has_value()) {
            patch.set_clause += std::string(name) + " = NULL";
            applied_fields.push_back(fmt::format("\"{}\": null", name));
            return true;
        }
        auto val = **opt;
        if (val < 0 || val > max_val) {
            patch.error = fmt::format("{} must be null or an integer in [0, {}]", name, max_val);
            return false;
        }
        patch.binds.push_back(std::to_string(val));
        patch.set_clause += fmt::format("{} = ${}::{}", name, patch.binds.size(), pg_type);
        applied_fields.push_back(fmt::format("\"{}\": {}", name, val));
        return true;
    };

    if (!check_field(0, "storage_limit", "bigint", INT64_MAX, req.storage_limit))
        return patch;
    if (!check_field(1, "entry_count_limit", "bigint", INT64_MAX, req.entry_count_limit))
        return patch;
    if (!check_field(2, "rate_limit_rpm", "int", INT32_MAX, req.rate_limit_rpm))
        return patch;
    if (!check_field(3, "max_file_size_mb", "bigint", INT64_MAX, req.max_file_size_mb))
        return patch;
    if (!check_field(4, "preferred_chunk_size_mb", "int", 1024, req.preferred_chunk_size_mb))
        return patch;
    if (!check_field(5, "rate_limit_upload_rpm", "int", INT32_MAX, req.rate_limit_upload_rpm))
        return patch;

    if (patch.set_clause.empty()) {
        patch.error = "no quota field supplied";
        return patch;
    }

    patch.applied_json = "{" + sgrn::utils::strings::join(applied_fields, ", ") + "}";
    return patch;
}

// t_key_column is "id" (users/services/domains) or "name" (organisations).
// Scope to the caller's organisation unless platform admin; soft-deleted rows
// are never updated. One audit row per successful update; missing and
// other-tenant targets produce the identical error (no tenant enumeration).
Task<HttpResponsePtr> applyQuotaUpdate(HttpRequestPtr tsp_req, const char* t_table, const char* t_key_column, std::string t_key_value,
    bool t_org_scoped, bool t_soft_deletable, const char* t_target_type, std::size_t t_column_count) {
    const auto ctx = detail::requestContext(tsp_req);
    auto req_res = parseTypedRequest<QuotaUpdateRequest>(tsp_req);
    if (req_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidPayload});
    }
    QuotaPatch patch = buildQuotaPatch(req_res.value(), t_column_count);
    if (!patch.error.empty()) {
        co_return createTypedResponse(BackendErrorResponse{patch.error, k400BadRequest, "AdminApi"});
    }

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
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
            co_return createTypedResponse(BackendErrorResponse{AdminApiError::InvalidUserId});
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return createTypedResponse(
            BackendErrorResponse{std::string("Quota update failed: ") + e.base().what(), k400BadRequest, "AdminApi"});
    }

    co_await sgrn::datastore::audit::AuditLogger::log(db, ctx.organisation, "user", ctx.actor_id, ctx.actor_name, "quota.updated",
        t_target_type, t_key_value, tsp_req->getPeerAddr().toIp(), sgrn::json::RawJson(patch.applied_json), "success");

    co_return createTypedResponse(tsp_req, QuotaUpdateResponse{sgrn::json::RawJson(patch.applied_json)}, k200OK);
}

} // namespace detail

Task<HttpResponsePtr> AdminApiHandler::handleUpdateOrgQuota(HttpRequestPtr tsp_req) {
    const auto ctx = detail::requestContext(tsp_req);
    std::string target = ctx.organisation;
    if (sgrn::datastore::isPlatformAdmin(tsp_req)) { // platform admin may name another org via ?name=
        const std::string named = tsp_req->getParameter("name");
        if (!named.empty()) {
            target = named;
        }
    }
    co_return co_await detail::applyQuotaUpdate(tsp_req, "core.organisations", "name", target, false, false, "organisation", 3);
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateUserQuota(HttpRequestPtr tsp_req, std::string t_id) {
    co_return co_await detail::applyQuotaUpdate(tsp_req, "core.users", "id", std::move(t_id), true, true, "user", 6);
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateServiceQuota(HttpRequestPtr tsp_req, std::string t_id) {
    co_return co_await detail::applyQuotaUpdate(
        tsp_req, "core.automated_services", "id", std::move(t_id), true, true, "automated_service", 6);
}

Task<HttpResponsePtr> AdminApiHandler::handleUpdateDomainQuota(HttpRequestPtr tsp_req, std::string t_id) {
    co_return co_await detail::applyQuotaUpdate(tsp_req, "core.domains", "id", std::move(t_id), true, false, "domain", 2);
}

} // namespace sgrn::datastore::handlers::admin
