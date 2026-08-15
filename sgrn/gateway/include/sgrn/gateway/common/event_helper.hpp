#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <set>
#include <vector>

namespace sgrn::gateway::common
{

/**
 * @brief Event filtering utilities for subscriptions
 */
namespace event_filter
{

/**
 * @brief Check if any dirty paths match subscriptions
 * @return Result with true if event should be sent, false if not, or error string
 */
inline bool shouldSend(const std::vector<sgrn::gateway::twin::TreePath>& t_dirty_paths, const std::set<std::string>& t_subscriptions) {

    if (t_subscriptions.empty()) {
        return true; // Firehose mode
    }

    for (const auto& dirty : t_dirty_paths) {
        std::string dirty_str = dirty.toDotted();

        for (const auto& sub : t_subscriptions) {
            // Convert slashes to dots for comparison
            std::string sub_dotted = sub;
            std::replace(sub_dotted.begin(), sub_dotted.end(), '/', '.');

            // Check if subscription matches dirty path
            if (dirty_str == sub_dotted || dirty_str.starts_with(sub_dotted + ".") || sub_dotted.starts_with(dirty_str + ".")) {
                return true;
            }
        }
    }

    return false; // No matches found - not an error, just no subscribers
}

/**
 * @brief Check if subscription needs field-level filtering
 * @return Result with true if filtering needed, false if not, or error string
 */
inline bool needsFieldFiltering(
    const std::vector<sgrn::gateway::twin::TreePath>& t_dirty_paths, const std::set<std::string>& t_subscriptions) {

    if (t_subscriptions.empty()) {
        return false; // Firehose - no filtering
    }

    bool has_db_level = false;
    bool has_field_level = false;

    for (const auto& sub : t_subscriptions) {
        bool is_field_level = sub.find('/') != std::string::npos;

        for (const auto& dirty : t_dirty_paths) {
            std::string dirty_str = dirty.toDotted();
            std::string sub_dotted = sub;
            std::replace(sub_dotted.begin(), sub_dotted.end(), '/', '.');

            if (dirty_str == sub_dotted || dirty_str.starts_with(sub_dotted + ".") || sub_dotted.starts_with(dirty_str + ".")) {

                if (is_field_level) {
                    has_field_level = true;
                } else {
                    has_db_level = true;
                }
                break;
            }
        }
    }

    // If any DB-level subscription matches, no filtering needed
    return has_field_level && !has_db_level;
}

} // namespace event_filter

} // namespace sgrn::gateway::common
