#include <sgrn/crud/AuditLogger.hpp>
#include <chrono>
#include <trantor/utils/Logger.h>

namespace sgrn::crud
{

void AuditLogger::logMutation(const std::string& table, const std::string& action, const std::string& tenant, const std::string& record_id,
    const Json::Value& payload, const std::string& user_id) {
    std::string body;
    body.reserve(256);
    body += R"({"timestamp":)";
    body +=
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    body += R"(,"table":")";
    body += table;
    body += R"(","action":")";
    body += action;
    body += R"(","tenant":")";
    body += tenant;
    body += R"(","record_id":")";
    body += record_id;
    if (!user_id.empty()) {
        body += R"(","user_id":")";
        body += user_id;
    }
    body += '"';
    if (!payload.isNull()) {
        Json::FastWriter writer;
        body += R"(,"payload":)";
        body += writer.write(payload);
    }
    body += '}';

    LOG_INFO << "[AUDIT] " << body;
}

} // namespace sgrn::crud
