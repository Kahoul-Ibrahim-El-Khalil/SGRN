#include <sgrn/datastore/audit/AuditLogger.hpp>

#include <drogon/HttpAppFramework.h>
#include <trantor/utils/Logger.h>

namespace sgrn::datastore::audit
{

drogon::Task<bool> AuditLogger::log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation, std::string t_actor_type,
    std::optional<int32_t> t_actor_id, std::string t_actor_name, std::string t_action, std::string t_target_type, std::string t_target_id,
    std::string t_ip, Json::Value t_details, std::string t_status) {
    try {
        if (!tsp_db_client) {
            tsp_db_client = drogon::app().getDbClient();
        }
        if (!tsp_db_client) {
            co_return false;
        }

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string details_str = Json::writeString(wb, t_details);

        co_await tsp_db_client->execSqlCoro(
            "INSERT INTO core.audit_logs "
            "(organisation, actor_type, actor_id, actor_name, action, target_type, target_id, ip, details, status) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8::inet, $9::jsonb, $10)",
            t_organisation, t_actor_type, t_actor_id, t_actor_name, t_action, t_target_type, t_target_id, t_ip.empty() ? "127.0.0.1" : t_ip,
            details_str, t_status);

        co_return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "AuditLogger failed to log action '" << t_action << "': " << e.what();
        co_return false;
    }
}

} // namespace sgrn::datastore::audit
