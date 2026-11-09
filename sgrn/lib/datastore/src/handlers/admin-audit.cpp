// admin-audit.cpp — Audit-log administration.
#include "admin-common.hpp"

namespace sgrn::datastore::handlers::admin
{
using namespace drogon;

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
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
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

        Json::Value logs_arr = Json::arrayValue;
        for (const auto& row : res) {
            Json::Value entry;
            entry["id"] = row["id"].as<int64_t>();
            entry["organisation"] = row["organisation"].as<std::string>();
            entry["actor_type"] = row["actor_type"].as<std::string>();
            entry["actor_id"] = row["actor_id"].isNull() ? Json::Value(Json::nullValue) : Json::Value(row["actor_id"].as<int32_t>());
            entry["actor_name"] = row["actor_name"].as<std::string>();
            entry["action"] = row["action"].as<std::string>();
            entry["target_type"] = row["target_type"].isNull() ? "" : row["target_type"].as<std::string>();
            entry["target_id"] = row["target_id"].isNull() ? "" : row["target_id"].as<std::string>();
            entry["ip"] = row["ip"].as<std::string>();
            entry["status"] = row["status"].as<std::string>();
            entry["created_at"] = row["created_at"].as<std::string>();

            const std::string raw_det = row["details"].isNull() ? "{}" : row["details"].as<std::string>();
            Json::CharReaderBuilder builder;
            Json::Value det_json;
            std::string errs;
            const std::unique_ptr<Json::CharReader> up_reader(builder.newCharReader());
            if (up_reader->parse(raw_det.data(), raw_det.data() + raw_det.size(), &det_json, &errs)) {
                entry["details"] = det_json;
            } else {
                entry["details"] = Json::objectValue;
            }
            logs_arr.append(entry);
        }

        Json::Value resp;
        resp["success"] = true;
        resp["audit_logs"] = logs_arr;
        resp["limit"] = limit;
        resp["offset"] = offset;
        co_return createJsonResponse(resp, k200OK);
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
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
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
        Json::Value details;
        details["older_than_days"] = days;
        details["records_purged"] = purged_count;
        details["cleared_by"] = actor_name;
        details["target_organisation"] = org_filter.empty() ? "all" : org_filter;
        details["status_message"] = purge_status_msg;

        const std::string log_org = org_filter.empty() ? "platform" : org_filter;
        co_await sgrn::datastore::audit::AuditLogger::log(db, log_org, "user", actor_id, actor_name, "audit.purged", "audit_logs", log_org,
            tsp_req->getPeerAddr().toIp(), details, "success");

        Json::Value resp;
        resp["success"] = true;
        resp["records_purged"] = purged_count;
        resp["older_than_days"] = days;
        resp["message"] = purge_status_msg.empty() ? "Audit logs purged successfully" : purge_status_msg;
        co_return createJsonResponse(resp, k200OK);
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

        Json::Value log_details;
        log_details["target_organisation"] = target_org.empty() ? "all" : target_org;
        log_details["is_valid"] = verify_res.is_valid;
        log_details["total_rows"] = static_cast<Json::Int64>(verify_res.total_rows);
        log_details["verified_checkpoints"] = static_cast<Json::Int64>(verify_res.verified_checkpoints);

        co_await sgrn::datastore::audit::AuditLogger::log(db, session_org, "user", actor_id, actor_name, "audit.verified", "audit_logs",
            target_org, tsp_req->getPeerAddr().toIp(), log_details, verify_res.is_valid ? "success" : "failure");

        Json::Value resp;
        resp["is_valid"] = verify_res.is_valid;
        resp["total_rows"] = static_cast<Json::Int64>(verify_res.total_rows);
        resp["verified_checkpoints"] = static_cast<Json::Int64>(verify_res.verified_checkpoints);
        resp["status"] = verify_res.is_valid ? "verified" : "tampered";
        resp["message"] = verify_res.failure_reason;
        if (!verify_res.is_valid) {
            if (verify_res.first_broken_seq.has_value()) {
                resp["first_broken_seq"] = static_cast<Json::Int64>(*verify_res.first_broken_seq);
            }
            if (verify_res.first_broken_id.has_value()) {
                resp["first_broken_id"] = static_cast<Json::Int64>(*verify_res.first_broken_id);
            }
        }

        co_return createJsonResponse(resp, verify_res.is_valid ? k200OK : k409Conflict);
    } catch (const std::exception& e) {
        ADMIN_ERROR_LOG("Failed to verify audit logs: {}", e.what());
        co_return createErrorResponse(std::string("Failed to verify audit logs: ") + e.what(), k400BadRequest, "AdminApi");
    }
}

} // namespace sgrn::datastore::handlers::admin
