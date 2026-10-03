#pragma once

#include <json/json.h>
#include <string>

namespace sgrn::crud
{

class AuditLogger {
public:
    static void logMutation(const std::string& table, const std::string& action, const std::string& tenant, const std::string& record_id,
        const Json::Value& payload = Json::Value::null, const std::string& user_id = "");
};

} // namespace sgrn::crud
