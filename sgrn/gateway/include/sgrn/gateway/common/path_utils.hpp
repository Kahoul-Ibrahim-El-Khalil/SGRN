#pragma once

#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/strings.hpp>
#include <optional>
#include <string>
#include <utility>

namespace sgrn::gateway::common
{

/**
 * @brief Path manipulation utilities for protocol adapters
 */
namespace path_utils
{
/**
 * @brief Convert topic path (slashes) to PLC path (dots)
 * @example "ReactorCore/speed" → "ReactorCore.speed"
 */
inline std::string topicToPlcPath(std::string_view t_topic) {
    std::string result;
    result.reserve(t_topic.size());
    for (char c : t_topic) {
        result += (c == '/') ? '.' : c;
    }
    return result;
}

/**
 * @brief Convert PLC path (dots) to topic path (slashes)
 * @example "ReactorCore.speed" → "ReactorCore/speed"
 */
inline std::string plcPathToTopic(std::string_view t_plc_path) {
    std::string result;
    result.reserve(t_plc_path.size());
    for (char c : t_plc_path) {
        result += (c == '.') ? '/' : c;
    }
    return result;
}

/**
 * @brief Parse a PLC path into DB number and field path
 * @example "ReactorCore.speed" → {1, "speed"} (if ReactorCore is DB1)
 * @example "DB1.speed" → {1, "speed"}
 * @return Result with pair<db_number, field_path> or error string
 */
inline sgrn::Result<std::pair<uint16_t, std::string>, std::string> parsePlcPath(
    std::string_view t_path, const sgrn::scl::PlcSchemaStore& t_store) {

    auto parts = sgrn::utils::strings::tokenize(t_path, '.');
    if (parts.empty()) {
        return "Empty path";
    }

    // First part is DB name or number
    std::optional<uint16_t> db_num;
    if (auto parsed = sgrn::utils::strings::parseInt(parts[0])) {
        db_num = *parsed;
    } else {
        // Look up by name
        for (uint16_t db : t_store.availableDbs()) {
            auto schema_res = t_store.getDb(db);
            if (!schema_res.hasError() && schema_res.value()->db_name == parts[0]) {
                db_num = db;
                break;
            }
        }
    }

    if (!db_num.has_value()) {
        return "DB not found: " + std::string(parts[0]);
    }

    // Remaining parts form the field path
    std::string field_path;
    if (parts.size() > 1) {
        field_path = sgrn::utils::strings::join(std::vector<std::string>(parts.begin() + 1, parts.end()), ".");
    }

    return std::make_pair(*db_num, field_path);
}

/**
 * @brief Build topic path from DB number and field path
 * @example {1, "speed"} → "DB1/speed"
 */
inline std::string buildTopic(uint16_t t_db_number, std::string_view t_field_path = "") {

    std::string topic = "DB" + std::to_string(t_db_number);
    if (!t_field_path.empty()) {
        topic += "/";
        topic += t_field_path;
    }
    return topic;
}

/**
 * @brief Build topic path from DB name and field path
 * @example {"ReactorCore", "speed"} → "ReactorCore/speed"
 */
inline std::string buildTopic(std::string_view t_db_name, std::string_view t_field_path = "") {

    std::string topic = std::string(t_db_name);
    if (!t_field_path.empty()) {
        topic += "/";
        topic += t_field_path;
    }
    return topic;
}

/**
 * @brief Convert topic path (slashes) to PLC path (dots) in-place
 * @example "ReactorCore/speed" → "ReactorCore.speed"
 */
inline void topicToPlcPathInPlace(std::string& t_path) {
    for (char& c : t_path) {
        if (c == '/')
            c = '.';
    }
}

/**
 * @brief Convert PLC path (dots) to topic path (slashes) in-place
 * @example "ReactorCore.speed" → "ReactorCore/speed"
 */
inline void plcPathToTopicInPlace(std::string& t_path) {
    for (char& c : t_path) {
        if (c == '.')
            c = '/';
    }
}
}; // namespace path_utils

} // namespace sgrn::gateway::common
