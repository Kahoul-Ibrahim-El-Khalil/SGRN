// admin-audit.cpp — Audit-log administration.
#include "admin-common.hpp"

#include <sgrn/utils/json-serialization.hpp>

#include <optional>

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

namespace detail
{

// The response is deliberately separate from AuditVerificationResult: this is
// the HTTP contract, while AuditVerificationResult is the datastore result.
struct AuditVerificationResponse {
    bool is_valid{false};
    int64_t total_rows{0};
    int64_t verified_checkpoints{0};
    std::string status;
    std::string message;
    std::optional<int64_t> first_broken_seq;
    std::optional<int64_t> first_broken_id;
};

struct AuditLogEntry {
    int64_t id{0};
    std::string organisation;
    std::string actor_type;
    std::optional<int32_t> actor_id;
    std::string actor_name;
    std::string action;
    std::string target_type;
    std::string target_id;
    std::string ip;
    std::string status;
    std::string created_at;
    sgrn::json::RawJson details{"{}"};
};

struct AuditLogsResponse {
    bool success{true};
    std::vector<AuditLogEntry> audit_logs;
    int32_t limit{0};
    int32_t offset{0};
};

struct AuditPurgeResponse {
    bool success{true};
    int64_t records_purged{0};
    int32_t older_than_days{0};
    std::string message;
};

struct AuditPurgeDetails {
    int32_t older_than_days{0};
    int64_t records_purged{0};
    std::string cleared_by;
    std::string target_organisation;
    std::string status_message;
};

struct AuditVerificationDetails {
    std::string target_organisation;
    bool is_valid{false};
    int64_t total_rows{0};
    int64_t verified_checkpoints{0};
};

} // namespace detail

} // namespace sgrn::datastore::handlers::admin

namespace sgrn::datastore::handlers::admin
{

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
        CO_RETURN_IF(db_res.hasError(), createJsonResponse(db_res));

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

        std::vector<detail::AuditLogEntry> audit_logs;
        audit_logs.reserve(res.size());
        for (const auto& row : res) {
            detail::AuditLogEntry entry;
            entry.id = row["id"].as<int64_t>();
            entry.organisation = row["organisation"].as<std::string>();
            entry.actor_type = row["actor_type"].as<std::string>();
            if (!row["actor_id"].isNull()) {
                entry.actor_id = row["actor_id"].as<int32_t>();
            }
            entry.actor_name = row["actor_name"].as<std::string>();
            entry.action = row["action"].as<std::string>();
            entry.target_type = row["target_type"].isNull() ? "" : row["target_type"].as<std::string>();
            entry.target_id = row["target_id"].isNull() ? "" : row["target_id"].as<std::string>();
            entry.ip = row["ip"].as<std::string>();
            entry.status = row["status"].as<std::string>();
            entry.created_at = row["created_at"].as<std::string>();
            if (!row["details"].isNull()) {
                entry.details = sgrn::json::RawJson{row["details"].as<std::string>()};
            }
            audit_logs.push_back(std::move(entry));
        }

        detail::AuditLogsResponse resp;
        resp.audit_logs = std::move(audit_logs);
        resp.limit = limit;
        resp.offset = offset;

        auto response_result = sgrn::json::write(resp);
        if (response_result.hasError()) {
            ADMIN_ERROR_LOG("Failed to serialize audit logs response: {}", response_result.error().what());
            co_return createErrorResponse("Failed to serialize audit logs response", k500InternalServerError, "AdminApi");
        }
        std::string response_json = std::move(response_result).value();
        co_return createJsonResponse(std::move(response_json), k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to fetch audit logs: {}", e.what());
        co_return createErrorResponse(std::string("Failed to fetch audit logs: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handlePurgeAuditLogs(HttpRequestPtr tsp_req) {
    try {
        if (!sgrn::datastore::isPlatformAdmin(tsp_req)) {
            co_return createErrorResponse(
                "Forbidden: Platform administrator privileges required to purge audit logs", k403Forbidden, "AdminApi");
        }

        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        const std::string days_raw = tsp_req->getParameter("older_than_days");
        int32_t days = days_raw.empty() ? 90 : std::stoi(days_raw);
        if (days < 90) {
            co_return createErrorResponse("Audit logs purge requires a minimum retention period of 90 days", k400BadRequest, "AdminApi");
        }

        const std::string org_filter = tsp_req->getParameter("organisation");

        auto db_res = sgrn::datastore::core::getDbClient();

        CO_RETURN_IF(db_res.hasError(), createJsonResponse(db_res));

        auto db = db_res.value();

        int64_t purged_count = 0;
        std::vector<std::string> orgs_to_purge;
        if (!org_filter.empty()) {
            orgs_to_purge.push_back(org_filter);
        } else {
            auto org_rows = co_await db->execSqlCoro("SELECT DISTINCT organisation FROM core.audit_logs ORDER BY organisation");
            for (const auto& r : org_rows) {
                orgs_to_purge.push_back(r["organisation"].as<std::string>());
            }
        }

        std::string purge_status_msg;
        for (const auto& target_org : orgs_to_purge) {
            auto res = co_await db->execSqlCoro(
                "SELECT purged_count, purged_up_to_seq, status_message FROM core.purge_audit_logs($1::text, ($2 || ' days')::interval)",
                target_org, std::to_string(days));
            if (!res.empty()) {
                purged_count += res[0]["purged_count"].as<int64_t>();
                if (purge_status_msg.empty()) {
                    purge_status_msg = res[0]["status_message"].as<std::string>();
                }
            }
        }

        // Auditable Clearance: Log an immutable audit entry documenting the purge operation!
        detail::AuditPurgeDetails details{
            .older_than_days = days,
            .records_purged = purged_count,
            .cleared_by = actor_name,
            .target_organisation = org_filter.empty() ? "all" : org_filter,
            .status_message = purge_status_msg,
        };

        const std::string log_org = org_filter.empty() ? "platform" : org_filter;
        co_await sgrn::datastore::audit::AuditLogger::log(db, log_org, "user", actor_id, actor_name, "audit.purged", "audit_logs", log_org,
            tsp_req->getPeerAddr().toIp(), details, "success");

        detail::AuditPurgeResponse resp;
        resp.records_purged = purged_count;
        resp.older_than_days = days;
        resp.message = purge_status_msg.empty() ? "Audit logs purged successfully" : purge_status_msg;

        auto response_result = sgrn::json::write(resp);
        if (response_result.hasError()) {
            ADMIN_ERROR_LOG("Failed to serialize audit purge response: {}", response_result.error().what());
            co_return createErrorResponse("Failed to serialize audit purge response", k500InternalServerError, "AdminApi");
        }
        std::string response_json = std::move(response_result).value();
        co_return createJsonResponse(std::move(response_json), k200OK);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to purge audit logs: {}", e.what());
        co_return createErrorResponse(std::string("Failed to purge audit logs: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

Task<HttpResponsePtr> AdminApiHandler::handleVerifyAuditLogs(HttpRequestPtr tsp_req) {
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        std::string target_org = session_org;
        const std::string q_org = tsp_req->getParameter("organisation");
        if (!q_org.empty()) {
            if (!is_platform && q_org != session_org) {
                co_return createErrorResponse("Forbidden: cannot verify audit logs of another organisation", k403Forbidden, "AdminApi");
            }
            target_org = q_org;
        } else if (is_platform) {
            target_org = ""; // verify all
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto verify_res = co_await sgrn::datastore::audit::AuditLogger::verifyChain(db, target_org);

        // Audit the verification run
        const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
        const int32_t actor_id = session["user"]["id"].asInt();
        const std::string actor_name = session["user"]["email"].asString();

        co_await sgrn::datastore::audit::AuditLogger::log(db, session_org, "user", actor_id, actor_name, "audit.verified", "audit_logs",
            target_org, tsp_req->getPeerAddr().toIp(),
            detail::AuditVerificationDetails{
                .target_organisation = target_org.empty() ? "all" : target_org,
                .is_valid = verify_res.is_valid,
                .total_rows = verify_res.total_rows,
                .verified_checkpoints = verify_res.verified_checkpoints,
            },
            verify_res.is_valid ? "success" : "failure");

        detail::AuditVerificationResponse resp;
        resp.is_valid = verify_res.is_valid;
        resp.total_rows = verify_res.total_rows;
        resp.verified_checkpoints = verify_res.verified_checkpoints;
        resp.status = verify_res.is_valid ? "verified" : "tampered";
        resp.message = verify_res.failure_reason;
        if (!verify_res.is_valid) {
            resp.first_broken_seq = verify_res.first_broken_seq;
            resp.first_broken_id = verify_res.first_broken_id;
        }

        auto response_result = sgrn::json::write(resp);
        if (response_result.hasError()) {
            ADMIN_ERROR_LOG("Failed to serialize audit verification response: {}", response_result.error().what());
            co_return createErrorResponse("Failed to serialize audit verification response", k500InternalServerError, "AdminApi");
        }
        std::string response_json = std::move(response_result).value();
        co_return createJsonResponse(std::move(response_json), verify_res.is_valid ? k200OK : k409Conflict);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to verify audit logs: {}", e.what());
        co_return createErrorResponse(std::string("Failed to verify audit logs: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
