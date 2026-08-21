#pragma once

#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::core
{

enum class SnapshotMode {
    Standalone, // Single snapshot per file (Legacy/Simple)
    Journaled,  // Multiple full snapshots in one file (Redundant)
    Anchored    // First full tree + subsequent deltas (Optimized)
};

inline SnapshotMode parseSnapshotMode(const std::string& t_s) {
    if (t_s == "Standalone" || t_s == "FULL_TREE")
        return SnapshotMode::Standalone;
    if (t_s == "Journaled" || t_s == "BATCH_OF_FULL_TREES")
        return SnapshotMode::Journaled;
    return SnapshotMode::Anchored;
}

inline const char* snapshotModeToString(SnapshotMode t_m) {
    switch (t_m) {
        case SnapshotMode::Standalone:
            return "Standalone";
        case SnapshotMode::Journaled:
            return "Journaled";
        case SnapshotMode::Anchored:
            return "Anchored";
    }
    return "Anchored";
}

/**
 * @brief Builds a high-performance JSON snapshot string from the server's DB buffers.
 * Uses rapidjson for zero-allocation serialization and supports granular recursive diffing.
 */
std::string buildSnapshotJson(sgrn::gateway::twin::PlcMemory& t_server, const std::map<uint16_t, const ::sgrn::scl::DbSchema*>& t_regs,
    const std::unordered_map<uint16_t, std::vector<std::pair<int32_t, int32_t>>>& t_filter);

} // namespace sgrn::gateway::core
