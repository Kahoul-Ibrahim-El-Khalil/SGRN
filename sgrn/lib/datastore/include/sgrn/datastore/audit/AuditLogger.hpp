#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>

#include <optional>
#include <string>

namespace sgrn::datastore::audit
{

class AuditLogger {
public:
    // Asynchronously log an event to core.audit_logs table
    static drogon::Task<bool> log(drogon::orm::DbClientPtr tsp_db_client, std::string t_organisation,
        std::string t_actor_type, // "user", "automated_service", "system"
        std::optional<int32_t> t_actor_id, std::string t_actor_name,
        std::string t_action,      // e.g. "auth.login", "user.created", "quota.updated", "audit.purged"
        std::string t_target_type, // "user", "automated_service", "role", "file", "organisation"
        std::string t_target_id, std::string t_ip, Json::Value t_details = Json::objectValue,
        std::string t_status = "success" // "success", "failure"
    );
};

} // namespace sgrn::datastore::audit
