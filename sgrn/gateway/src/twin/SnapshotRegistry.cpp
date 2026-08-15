#include <fmt/core.h>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/SnapshotRegistry.hpp>

namespace sgrn::gateway::twin
{

void SnapshotRegistry::registerSnapshot(uint16_t t_db_number, DbSnapshot* tp_snapshot) {
    if (!tp_snapshot)
        return;
    std::lock_guard lock(snapshot_index_mutex_);
    snapshot_index_[t_db_number] = tp_snapshot;
}

void SnapshotRegistry::unregisterSnapshot(uint16_t t_db_number) {
    std::lock_guard lock(snapshot_index_mutex_);
    snapshot_index_.erase(t_db_number);
}

void SnapshotRegistry::patchSnapshotRegion(uint16_t t_db_number, size_t t_offset, const uint8_t* tp_src, size_t t_bytes) {
    DbSnapshot* p_snap = nullptr;
    {
        std::lock_guard lock(snapshot_index_mutex_);
        auto it = snapshot_index_.find(t_db_number);
        if (it == snapshot_index_.end())
            return;
        p_snap = it->second;
    }
    p_snap->updateRawRegion(t_offset, tp_src, t_bytes);
}

} // namespace sgrn::gateway::twin
